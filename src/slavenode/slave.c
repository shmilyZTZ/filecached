/**************************************************
 * Created: 2013/4/11
 * Author: Leo_xy
 * Email: xy198781@sina.com
 * Last modified: 2013/4/21 23：00
 * Version: 0.1
 * File: src/slave.c
 * Breif: slave节点程序代码。
 **************************************************/

#include <pthread.h>
#include <mpi.h>
#include <time.h>

#include "slave.h"
#include "../common/common.h"
#include "../common/log.h"
#include "../common/tag.h"
#include "../common/request.h"
#include "../common/hashtable.h"
#include "../common/constants.h"
#include "mem_manage.h"

pthread_mutex_t lock_share_files_slave = PTHREAD_MUTEX_INITIALIZER; /* 保护share_file_slave哈希表的互斥量 */
HASH_TABLE(share_files_slave) = {}; /* 共享文件哈希表，保存共享文件元数据 */
pthread_mutex_t lock_private_files = PTHREAD_MUTEX_INITIALIZER; /* 保护private_files哈希标的互斥量 */
HASH_TABLE(private_files) = {}; /* 私有文件哈希表 */
static pthread_t tid[SLAVE_THREAD_NUM] = {}; /* slave节点线程号 */
tag_pool slave_tags = {};                    /* 请求用tag池 */
pthread_mutex_t lock_slave_tags = PTHREAD_MUTEX_INITIALIZER; /* tag池的保护互斥量 */
managememory manager;           /* 内存管理器 */
struct slave_info loc_slave;    /* 本地slave信息 */

/*初始化本地slave信息*/
static inline int 
init_slave_info(void)
{
    loc_slave.alive = 1;
    loc_slave.id = get_process_id();
    loc_slave.free  = manager.freesize;
    loc_slave.last_update = time(NULL);
    loc_slave.connections = 0;

    return 0;
}


/*slave报告自身信息*/
static int 
report_slave_info(void)
{
    /* 准备相应mpi类型 */
    MPI_Datatype mpi_request_type;
    MPI_Datatype mpi_slave_info_type;
    struct request req;
    build_mpi_type_request(&req, &mpi_request_type);
    build_mpi_type_slave_info(&loc_slave, &mpi_slave_info_type);

    /* 设置request */
    req.request = SLAVE_REPORT;
    int ret = get_tag(slave_tags, &lock_slave_tags);
    if (ret < 0) {
        LOG_MSG("IN FUNC report_slave_info: ERROR: tag can't be assigned!\n");
        return ret;
    }
    req.tag = ret;

    /* 计算缓冲大小 */
    int size = 0, sum_size = 0;
    MPI_Pack_size(1, mpi_request_type, MPI_COMM_WORLD, &size);
    sum_size += size;
    MPI_Pack_size(1, mpi_slave_info_type, MPI_COMM_WORLD, &size);
    sum_size += size;

    /* 装填缓冲区 */
    char buff[sum_size];
    int position  = 0;
    MPI_Pack(&req, 1, mpi_request_type, buff, sum_size, &position, MPI_COMM_WORLD);
    MPI_Pack(&loc_slave, 1, mpi_slave_info_type, buff, sum_size, &position, MPI_COMM_WORLD);

    /* 发送slave report */
    LOG_MSG("IN FUNC report_slave_info: INFO: slave report sending!\n");
    MPI_Send(buff, position, MPI_PACKED, 0, REQUEST_TAG, MPI_COMM_WORLD);
    LOG_MSG("IN FUNC report_slave_info: INFO: slave info report sended!\n");

    /* 接收确认 */
    MPI_Status stat;
    struct request req_ack;
    MPI_Recv(&req_ack, 1, mpi_request_type, 0, req.tag, MPI_COMM_WORLD, &stat);
    if (req_ack.tag != req.tag || req_ack.request != req.request) {
        LOG_MSG("IN FUNC report_slave_info: ERROR: wrong request ack received!\n");
        return -2;
    }
    LOG_MSG("IN FUNC report_slave_info: INFO: report ack received!\n");

    release_tag(slave_tags, req.tag, &lock_slave_tags);
    return 0;
}

/*slave存储共享数据映射表*/
static int
receive_share_file_map(void)
{
    /* 构造mpi share file类型 */
    struct share_file file_req;
    MPI_Datatype mpi_share_file_type;
    build_mpi_type_share_file(&file_req, &mpi_share_file_type);

    /* 试探并接收master发送的share file信息数据 */
    MPI_Status status;
    MPI_Message message;
    int buff_size;
    LOG_MSG("IN FUNC receive_share_file_map: INFO: probing master message!\n");
    MPI_Mprobe(0, SHARE_FILE_DIS_TAG, MPI_COMM_WORLD, &message, &status);
    MPI_Get_count(&status, MPI_PACKED, &buff_size);
    char buff[buff_size];
    LOG_MSG("IN FUNC receive_share_file_map: INFO: receiving master message!\n");
    MPI_Mrecv(buff, buff_size, MPI_PACKED, &message, &status);
    LOG_MSG("IN FUNC receive_share_file_map: INFO: master message received!\n");

    /* 提取share file信息，并插入哈希表 */
    int position = 0;
    while(position < buff_size)
    {
        struct share_file *file = (struct share_file *)malloc(sizeof(struct share_file));
        if (file == NULL) {
            LOG_MSG("IN FUNC receive_share_file_map: ERROR: malloc fail for struct share_file!\n");
            return -1;
        }
        MPI_Unpack(buff, buff_size, &position, file, 1, mpi_share_file_type, MPI_COMM_WORLD);
        if(hash_insert(&file->hnode, share_files_slave) != &file->hnode)
        {
            LOG_MSG("IN FUN handle_share_file_maps: ERROR: share_files table insert failed!\n");
            return -2;
        }
    }
    LOG_MSG("IN FUN handle_share_file_maps: INFO: share_files table constructed!\n");
    
    return 0;
}

/*slave远端数据的读取*/
int
slave_remote_read(const char *file, int slave_id,unsigned long pos,
                  unsigned long size, char *buf)
{
    int ret = 0;

    /* 构造相应mpi类型 */
    struct request req;
    MPI_Datatype mpi_request_type;
    build_mpi_type_request(&req, &mpi_request_type);

    /* 初始化request */
    req.request = REMOTE_READ;
    ret = get_tag(slave_tags, &lock_slave_tags);
    if (ret < 0) {
        LOG_MSG("IN FUNC slave_remote_read: ERROR: failed to get tag!\n");
        return ret;
    }
    req.tag = ret;
    
    /* 计算缓冲区长度 */
    int s = 0, sum = 0;
    MPI_Pack_size(1, mpi_request_type, MPI_COMM_WORLD, &s);
    sum += s;
    MPI_Pack_size(MAX_PATH_LENGTH, MPI_CHAR, MPI_COMM_WORLD, &s);
    sum += s;
    MPI_Pack_size(2, MPI_UNSIGNED_LONG, MPI_COMM_WORLD, &s);
    sum += s;

    /* 填充缓冲区 */
    char buff[sum];
    int position  = 0;
    unsigned long size_count[2] = {pos, size};
    MPI_Pack(&req, 1, mpi_request_type, buff, sum, &position, MPI_COMM_WORLD);
    MPI_Pack(file, MAX_PATH_LENGTH, MPI_CHAR, buff, sum, &position, MPI_COMM_WORLD);
    MPI_Pack(size_count, 2, MPI_UNSIGNED_LONG, buff, sum, &position, MPI_COMM_WORLD);

    /* 发送请求 */
    LOG_MSG("IN FUNC slave_remote_read: INFO: sending remote read request!\n");
    MPI_Send(buff, position, MPI_PACKED, slave_id, REQUEST_TAG, MPI_COMM_WORLD);
    LOG_MSG("IN FUNC slave_remote_read: INFO: remote read request sended!\n");

    /* 接收返回数据 */
    MPI_Status stat;
    int recv_count = 0;
    MPI_Recv(&buf, size, MPI_CHAR, slave_id, req.tag, MPI_COMM_WORLD,&stat);
    if (MPI_Get_count(&stat, MPI_CHAR, &recv_count) != size) {
        LOG_MSG("IN FUNC slave_remote_read: ERROR: data return error!\n");
        return recv_count;
    }
    LOG_MSG("IN FUNC slave_remote_read: INFO: remote read result received!\n");
    release_tag(slave_tags, req.tag, &lock_slave_tags);
    return recv_count;
}

/*slave载入数据*/
static int
handle_load_request(struct handler_param *handler_param)
{
    int ret = 0;
    
    /* 构造相应mpi类型 */
    struct block block;
    MPI_Datatype mpi_request_type;
    MPI_Datatype mpi_block_type;
    build_mpi_type_request(&handler_param->request, &mpi_request_type);
    build_mpi_type_block(&block, &mpi_block_type);
    
    char file[MAX_PATH_LENGTH];

    /* 解包相应数据 */
    MPI_Unpack(handler_param->buff, handler_param->buff_size, &handler_param->position,
               file, MAX_PATH_LENGTH, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Unpack(handler_param->buff, handler_param->buff_size, &handler_param->position,
               &block, 1, mpi_block_type, MPI_COMM_WORLD);

    /* 为文件分配内存 */
    ret = mem_malloc(&manager, file, block.size);
    if(ret != 0)
    {
        LOG_MSG("IN FUN handle_load_request ERROR: file memory allocation failed!\n");
        return ret;
    }
    LOG_MSG("IN FUN handle_load_request INFO: file memory allocation succeed!\n");

    /* 写入文件数据 */
    ret = mem_write_block(file, &block);
    if(ret != 0)
    {
        printf("IN FUN handle_load_request ERROR: file load failed!\n");
        return ret;
    }
    LOG_MSG("IN FUN handle_load_request INFO: file loaded into memory!\n");

    /* 发送反馈request */
    MPI_Send(&handler_param->request, 1, mpi_request_type, 0, handler_param->request.tag, MPI_COMM_WORLD);
    LOG_MSG("IN FUN handle_load_request INFO: request ack sended!\n");
    return 0;
}

/*slave处理远端数据请求*/
static int
handle_remote_read(struct handler_param *handler_param)
{
    int ret = 0;
    unsigned long size_count[2] = {};
    char file[MAX_PATH_LENGTH];

    /* 解包文件名，读取长度信息 */
    MPI_Unpack(handler_param->buff, handler_param->buff_size, &handler_param->position,
               file, MAX_PATH_LENGTH, MPI_CHAR, MPI_COMM_WORLD);
    MPI_Unpack(handler_param->buff, handler_param->buff_size, &handler_param->position,
               size_count, 2, MPI_UNSIGNED_LONG, MPI_COMM_WORLD);
   
    char buf[size_count[1]];

    /* 读取本地数据 */
    ret = mem_read(file, size_count[0], size_count[1], buf);
    if (ret != size_count[1]) {
        LOG_MSG("IN FUNC handle_remote_read: ERROR: requested data not fully readed!\n");
    } else {
        LOG_MSG("IN FUNC handle_remote_read: INFO: data readed from local memory\n");
    }

    /* 发送回请求的数据 */
    MPI_Send(buf, ret, MPI_CHAR, handler_param->status.MPI_SOURCE, handler_param->request.tag, MPI_COMM_WORLD);
    LOG_MSG("IN FUNC handle_remote_read: INFO: requested data sended!\n");

    return 0;
}

/* 请求处理函数入口数组 */
static request_handler request_handlers[MAX_REQUEST_TYPES] = {
    0,
    handle_load_request,
    handle_remote_read
};

/*slave消息处理线程*/
static void *
slave_listen_thread(void *param)
{
    ssize_t ret = 0;

    /* 构造request类型 */
    struct handler_param handler_param;    
    build_mpi_type_request(&handler_param.request, &handler_param.mpi_request_type);

    /* 设置log id */
    ret = set_log_id((ssize_t)param);
    if (ret != 0) {
        LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave failed to set log id!\n", loc_slave.id, (ssize_t)param);
        return (void *)ret;
    }
    
    /* 试探消息长度 */
    MPI_Message message;
    MPI_Mprobe(MPI_ANY_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, &message, &handler_param.status);
    MPI_Get_count(&handler_param.status, MPI_PACKED, &handler_param.buff_size);
    LOG_MSG("IN SLAVE %d THREAD %d: INFO: request message probed!\n", loc_slave.id, (ssize_t)param);
    
    /* 请求处理循环 */
    while (1) {
        char buff[handler_param.buff_size];
        handler_param.position = 0;
        handler_param.buff = buff;

        /* 根据试探长度，接收消息 */
        MPI_Mrecv(handler_param.buff, handler_param.buff_size, MPI_PACKED, &message, &handler_param.status);
        LOG_MSG("IN SLAVE %d THREAD %d: INFO: request message received!\n", loc_slave.id, (ssize_t)param);

        /* 解包struct request结构体信息 */
        MPI_Unpack(handler_param.buff, handler_param.buff_size, &handler_param.position, &handler_param.request,
                   1, handler_param.mpi_request_type, MPI_COMM_WORLD);
        LOG_MSG("IN SLAVE %d THREAD %d: INFO: requst %d tag %d received!", loc_slave.id, (ssize_t)param,
                handler_param.request.request, handler_param.request.tag);

        /* 调用请求处理函数执行处理过程 */
        ret = request_handlers[handler_param.request.request](&handler_param);
        if (ret != 0) {
            LOG_MSG("IN SLAVE %d THREAD %d: ERROR: requst %d tag %d failed handling!", loc_slave.id, (ssize_t)param,
                    handler_param.request.request, handler_param.request.tag);
        } else {
            LOG_MSG("IN SLAVE %d THREAD %d: INFO: requst %d tag %d handled!", loc_slave.id, (ssize_t)param,
                    handler_param.request.request, handler_param.request.tag);
        }

        /* 试探消息长度 */
        MPI_Mprobe(MPI_ANY_SOURCE, REQUEST_TAG, MPI_COMM_WORLD, &message, &handler_param.status);
        MPI_Get_count(&handler_param.status, MPI_PACKED, &handler_param.buff_size);
        LOG_MSG("IN SLAVE %d THREAD %d: INFO: request message probed!\n", loc_slave.id, (ssize_t)param);
    }
    return (void *)0;
}

/*slave主服务线程*/
static void *
slave_main_thread(void *param)
{
    ssize_t ret = 0;

    /* 设置log id */
    ret = set_log_id((ssize_t)param);
    if (ret != 0) {
        LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave failed to set log id!\n", loc_slave.id, (ssize_t)param);
        return (void *)ret;
    }

    /* 创建监听线程 */
    LOG_MSG("IN SLAVE %d THREAD %d: INFO: creating listenning thread!\n", loc_slave.id, (ssize_t)param);
    ssize_t i;
    for (i = 1; i < SLAVE_THREAD_NUM; i++) {
        ret = pthread_create(&tid[i], NULL, slave_listen_thread, (void *)i);
        if (ret != 0) {
            LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave listenning thread failed to be created!\n", loc_slave.id, (ssize_t)param);
            return (void *)ret;
        }
    }
    LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave listenning thread created!\n", loc_slave.id, (ssize_t)param);
    
    /* 等待监听线程结束 */
    for (i = 1; i < SLAVE_THREAD_NUM; i++) {
        ret = pthread_join(tid[i], NULL);
        if (ret != 0) {
            LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave listenning thread failed to be stopped!\n", loc_slave.id, (ssize_t)param);
            return (void *)ret;
        }
    }
    LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave listenning thread stopped!\n", loc_slave.id, (ssize_t)param);

    /* 关闭日志文件 */
    ret = close_logger(SLAVE_THREAD_NUM);
    if (ret != 0) {
        LOG_MSG("IN SLAVE %d THREAD %d: INFO: slave logger failed to be closed!\n", loc_slave.id, (ssize_t)param);
    }
    return (void *)ret;
}

/*初始化slave，并创建相关线程*/
int
init_dmf_slave(unsigned long mem)
{
    int ret = 0;

    /* 初始化日志文件及线程日志编号 */
    ret = init_logger("./dmf_log/slave", SLAVE_THREAD_NUM);
    if (ret != 0 || set_log_id((void *)MAX_THREAD_NUM)) {
        printf("IN FUN init_dmf_slave: ERROR: logger initialization failed!\n NOTICE: LOG DIRCTORY MUST EXISTS!\n");
        return ret;
    }
    LOG_MSG("IN FUNC init_dmf_slave: INFO: slave initializing!\n");

    /* 初始化内存池 */
    ret = mem_init(&manager, mem);
    if (ret != 0) {
        LOG_MSG("IN FUNC init_dmf_slave: ERROR: memory allocation failed!\n");
        return ret;
    }
    LOG_MSG("IN FUNC init_dmf_slave: INFO: memory pool allocated!\n");

    /* 初始化本节点信息 */
    ret = init_slave_info();
    if (ret != 0) {
        LOG_MSG("IN FUNC init_dmf_slave: ERROR: slave info initialization failed!\n");
        return ret;
    }
    LOG_MSG("IN FUNC init_dmf_slave: INFO: slave info updated!\n");
    
    /* 创建slave各服务线程 */
    ret = pthread_create(&tid[0], NULL, slave_main_thread, 0);
    if (ret != 0) {
        LOG_MSG("IN FUNC init_dmf_slave: ERROR: failed to init slave threads!\n");
        return ret;
    }
    LOG_MSG("IN FUNC init_dmf_slave: INFO: slave thread created!\n");
    
    /* 发送slave信息 */
    ret = report_slave_info();
    if (ret != 0) {
        LOG_MSG("IN FUNC init_dmf_slave: ERROR: failed to report slave info!\n");
        return ret;
    }
    LOG_MSG("IN FUNC init_dmf_slave: INFO: slave info reported!\n");

    /* 接收共享文件元数据 */
    ret = receive_share_file_map();
    if (ret != 0) {
        LOG_MSG("IN FUNC init_dmf_slave: ERROR: failed to receive share file map!\n");
    }
    LOG_MSG("IN FUNC init_dmf_slave: INFO: share file map received!\n");
    return ret;
}

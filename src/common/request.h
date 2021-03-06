/**************************************************
 * Created: 2013/3/24
 * Author: Leo_xy
 * Email: xy198781@sina.com
 * Last modified: 2013/4/15 22:00
 * Version: 0.1
 * File: src/masternode/request.h
 * Breif: Header for request handling mechanism.
 **************************************************/

#ifndef REQUEST_HEADER
#define REQUEST_HEADER

#include <mpi.h>

#include "constants.h"
#include "common.h"

enum request_type {
    SLAVE_REPORT = 0,
    BLOCK_LOAD_REQ,
    REMOTE_READ   
};

struct request
{
    enum request_type request;
    int tag;
};

/* MPI包装share_files时传递的参数 */
struct pack_param
{
    MPI_Datatype mpi_share_file_type;
    char *buff;
    int position;
    int size;
};

struct handler_param
{
    MPI_Datatype mpi_request_type;
    struct request request;
    MPI_Status status;
    char *buff;
    int buff_size;
    int position;
};

typedef int (* request_handler)(struct handler_param *);

/* 对于struct request可以直接发送MPI_INT*2 */
void
build_mpi_type_request(struct request *, MPI_Datatype *);

/* MPI对应的struct slave_info_req类型 */
void
build_mpi_type_slave_info(struct slave_info *, MPI_Datatype *);

/* MPI对应的struct block_req类型 */
void
build_mpi_type_block(struct block *, MPI_Datatype *);

/* MPI对应的struct share_file_req类型 */
void
build_mpi_type_share_file(struct share_file *, MPI_Datatype *);

#endif

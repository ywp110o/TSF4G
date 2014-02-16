#include "tlibc/platform/tlibc_platform.h"
#include "tconnd/tdtp_instance.h"
#include "tcommon/tdgi_types.h"
#include "tcommon/tdgi_writer.h"
#include "tcommon/tdgi_reader.h"

#include "tconnd/tdtp_socket.h"
#include "tbus/tbus.h"


#include "tlibc/protocol/tlibc_binary_reader.h"
#include "tlibc/protocol/tlibc_binary_writer.h"
#include "tlibc/core/tlibc_mempool.h"
#include "tlibc/core/tlibc_timer.h"
#include "tlibc/core/tlibc_list.h"


#include "tconnd/globals.h"
#include "tconnd/tlog_instance.h"
#include "tconnd/tconnd_config.h"
#include "tlog/tlog.h"



#include <string.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>

#include "tconnd_tbus.h"

#include <errno.h>
#include <assert.h>
#include <stdio.h>

#include "tconnd/tconnd_timer.h"
#include "tconnd/tconnd_epoll.h"
#include "tconnd/tconnd_listen.h"
#include "tconnd/tconnd_signal.h"


TERROR_CODE tdtp_instance_init(tdtp_instance_t *self)
{
    int nb = 1;    
    if(tconnd_config_init(g_config_file) != E_TS_NOERROR)
    {
        goto ERROR_RET;
    }

    if(tlog_init(&g_tlog_instance, g_config.log_config) != E_TS_NOERROR)
    {
        INFO_PRINT("tlog init [%s] failed.", g_config.log_config);
        goto ERROR_RET;
    }
    INFO_PRINT("tlog init [%s] succeed.", g_config.log_config);
    INFO_LOG("tlog init [%s] succeed.", g_config.log_config);

    g_tdtp_instance_switch = FALSE;
    //warning!
    g_head_size = 16;

    if(signal_processing_init() != E_TS_NOERROR)
    {
        goto ERROR_RET;
    }
    

	self->listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if(self->listenfd == -1)
	{
		goto ERROR_RET;
	}

	
	if(ioctl(self->listenfd, FIONBIO, &nb) == -1)
	{	    
		goto close_listenfd;
	}

	
	memset(&self->listenaddr, 0, sizeof(struct sockaddr_in));

	self->listenaddr.sin_family=AF_INET;
	self->listenaddr.sin_addr.s_addr = inet_addr(g_config.ip);
	self->listenaddr.sin_port = htons(g_config.port);

	if(bind(self->listenfd,(struct sockaddr *)(&self->listenaddr), sizeof(struct sockaddr_in)) == -1)
	{
		goto close_listenfd;
	}	

	if(listen(self->listenfd, g_config.backlog) == -1)
	{
		goto close_listenfd;
	}


	self->epollfd = epoll_create(g_config.connections);
	if(self->epollfd == -1)
	{
		goto close_epollfd;
	}

	self->input_tbusid = shmget(g_config.input_tbuskey, 0, 0666);
	if(self->input_tbusid == -1)
	{
		goto close_epollfd;
	}
	self->input_tbus = shmat(self->input_tbusid, NULL, 0);
	if(self->input_tbus == NULL)
	{
		goto close_epollfd;
	}

	self->output_tbusid = shmget(g_config.output_tbuskey, 0, 0666);
	if(self->output_tbusid == -1)
	{
		goto input_dt;
	}
	self->output_tbus = shmat(self->output_tbusid, NULL, 0);
	if(self->output_tbus == NULL)
	{
		goto input_dt;
	}

	tlibc_timer_init(&self->timer, 0);
	self->timer_start_ms = get_current_ms();

	self->socket_pool = (tlibc_mempool_t*)malloc(
		TLIBC_MEMPOOL_SIZE(sizeof(tdtp_socket_t), g_config.connections));
	if(self->socket_pool == NULL)
	{
		goto output_dt;
	}
	self->socket_pool_size = tlibc_mempool_init(self->socket_pool, 
        TLIBC_MEMPOOL_SIZE(sizeof(tdtp_socket_t), g_config.connections)
        , sizeof(tdtp_socket_t));
	assert(self->socket_pool_size == g_config.connections);



	self->package_pool = (tlibc_mempool_t*)malloc(
		TLIBC_MEMPOOL_SIZE(sizeof(package_buff_t), MAX_PACKAGE_NUM));
	if(self->package_pool == NULL)
	{
		goto free_socket_pool;
	}
	self->package_pool_size = tlibc_mempool_init(self->package_pool, 
        TLIBC_MEMPOOL_SIZE(sizeof(package_buff_t), MAX_PACKAGE_NUM)
        , sizeof(package_buff_t));
	assert(self->package_pool_size == MAX_PACKAGE_NUM);


	tlibc_list_init(&self->readable_list);

    INFO_PRINT("tconnd init succeed.");
    INFO_LOG("tconnd init succeed.");
	return E_TS_NOERROR;
free_socket_pool:
    free(self->socket_pool);
output_dt:
    shmdt(self->output_tbus);
input_dt:
    shmdt(self->input_tbus);
close_epollfd:
    close(self->epollfd);
close_listenfd:
    close(self->listenfd);
ERROR_RET:
	return E_TS_ERROR;
}

TERROR_CODE tdtp_instance_process(tdtp_instance_t *self)
{
	TERROR_CODE ret = E_TS_WOULD_BLOCK;
	TERROR_CODE r;
	TLIBC_ERROR_CODE tlibc_ret;

	r = process_listen(self);
	if(r == E_TS_NOERROR)
	{
		ret = E_TS_NOERROR;
	}
	else if(r != E_TS_WOULD_BLOCK)
	{
		ret = r;
		goto done;
	}

	r = process_epool(self);
	if(r == E_TS_NOERROR)
	{
		ret = E_TS_NOERROR;
	}
	else if(r != E_TS_WOULD_BLOCK)
	{
		ret = r;
		goto done;
	}

    r = process_input_tbus(self);
	if(r == E_TS_NOERROR)
	{
		ret = E_TS_NOERROR;
	}
	else if(r != E_TS_WOULD_BLOCK)
	{
		ret = r;
		goto done;
	}

	r = signal_processing_proc();
	if(r == E_TS_NOERROR)
	{
	    ret = E_TS_NOERROR;
	}
	else if(r != E_TS_WOULD_BLOCK)
	{
		ret = r;
		goto done;
	}

    tlibc_ret = tlibc_timer_tick(&self->timer, tdtp_instance_get_time_ms(self));
    if(tlibc_ret == E_TLIBC_NOERROR)
    {
        ret = E_TS_NOERROR;
    }
    else if(tlibc_ret != E_TLIBC_WOULD_BLOCK)
    {
        ret = E_TS_ERROR;
        goto done;
    }
	
done:
	return ret;
}


void tdtp_instance_loop(tdtp_instance_t *self)
{
    tuint32 idle_count = 0;
    TERROR_CODE ret;

    g_tdtp_instance_switch = TRUE;
	for(;g_tdtp_instance_switch;)
	{
		ret = tdtp_instance_process(self);
		switch(ret)
		{
		case E_TS_NOERROR:
    		idle_count = 0;
		    break;
		case E_TS_WOULD_BLOCK:
    		{
    			++idle_count;
    			if(idle_count > 30)
    			{
    				usleep(1000);
    				idle_count = 0;
    			}
    			else
    			{
    				sched_yield();
    			}
	    	}
		    break;
		default:
        	goto done;
		}
	}

done:
	return;	
}

void tdtp_instance_fini(tdtp_instance_t *self)
{
    int i;

    shmdt(self->input_tbus);
    shmdt(self->output_tbus);
    
    for(i = self->socket_pool->used_head; i < self->socket_pool->unit_num; )
    {
        tlibc_mempool_block_t *b = TLIBC_MEMPOOL_GET_BLOCK(self->socket_pool, i);
        tdtp_socket_t *s = (tdtp_socket_t *)&b->data;
        tdtp_socket_free(s);

        i = b->next;
    }
        

    free(self->socket_pool);
    free(self->package_pool);

    close(self->listenfd);
    close(self->epollfd);
}


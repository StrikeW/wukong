#ifndef RDMARESOURCE_H
#define RDMARESOURCE_H

#include "network_node.h"

#pragma GCC diagnostic warning "-fpermissive"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <endian.h>
#include <byteswap.h>
#include <getopt.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "timer.h"

#include <vector>
#include <pthread.h>
struct config_t
{
  const char *dev_name;         /* IB device name */
  char *server_name;            /* server host name */
  u_int32_t tcp_port;           /* server TCP port */
  int ib_port;                  /* local IB port to work with */
  int gid_idx;                  /* gid index to use */
};

/* structure to exchange data which is needed to connect the QPs */
struct cm_con_data_t
{
  uint64_t addr;                /* Buffer address */
  uint32_t rkey;                /* Remote key */
  uint32_t qp_num;              /* QP number */
  uint16_t lid;                 /* LID of the IB port */
  uint8_t gid[16];              /* gid */
} __attribute__ ((packed));
/* structure of system resources */

struct dev_resource {
  struct ibv_device_attr device_attr;   /* Device attributes */
  struct ibv_port_attr port_attr;       /* IB port attributes */
  struct ibv_context *ib_ctx;   /* device handle */

  struct ibv_pd *pd;            /* PD handle */
  struct ibv_mr *mr;            /* MR handle for buf */
  char *buf;                    /* memory buffer pointer, used for RDMA and send*/

};

struct QP {
  struct cm_con_data_t remote_props;  /* values to connect to remote side */
  struct ibv_pd *pd;            /* PD handle */
  struct ibv_cq *cq;            /* CQ handle */
  struct ibv_qp *qp;            /* QP handle */
  struct ibv_mr *mr;            /* MR handle for buf */

  struct dev_resource *dev;
  
};


struct normal_op_req 
{
  ibv_wr_opcode opcode;
  char *local_buf;
  int size; //default set to sizeof(uint64_t)
  int remote_offset;
  
  //for atomicity operations
  uint64_t compare_and_add;
  uint64_t swap;
  
  //for internal usage!!
  struct ibv_send_wr sr;
  struct ibv_sge sge;
  
};

  struct per_thread_metadata{
    int prev_recv_tid;
    int prev_recv_mid;
    uint64_t own_count;
    uint64_t recv_count;
    uint64_t steal_count[40];
    pthread_spinlock_t recv_lock;
    char padding1[64];
    volatile bool need_help;
    char padding2[64];
    void lock(){
      pthread_spin_lock(&recv_lock);
    }
    bool trylock(){
      return pthread_spin_trylock(&recv_lock);
    }
    void unlock(){
      pthread_spin_unlock(&recv_lock);
    }
    per_thread_metadata(){
      need_help=false;
      pthread_spin_init(&recv_lock,0);
      own_count=0;
      recv_count=0;
      for(int i=0;i<40;i++){
        steal_count[i]=0;
      }
    }
  };
  class RdmaResource {

    //site configuration settings
    int _total_partition = -1;
    int _total_threads = -1;
    int _current_partition = -1;
  
    per_thread_metadata local_meta[40];
    struct dev_resource *dev0;//for remote usage
    struct dev_resource *dev1;//for local usage
    
    struct QP **res;
    struct QP  *own_res;
    
    uint64_t size;//The size of the rdma region,should be the same across machines!
    uint64_t off ;//The offset to send message
    char *buffer;
    
    int rdmaOp(int t_id,int m_id,char*buf,uint64_t size,uint64_t off,ibv_wr_opcode op) ;
    int batch_rdmaOp(int t_id,int m_id,char*buf,uint64_t size,uint64_t off,ibv_wr_opcode op) ;
    
    void init();
    
  public:
    uint64_t get_memorystore_size(){
      //[0-off) can be used;
      //[off,size) should be reserve
      return off;
    }
    char * get_buffer(){
      return buffer;
    }
    uint64_t get_slotsize(){
      return slotsize;
    }
    //rdma location hashing
    uint64_t slotsize;
    uint64_t rbf_size;
    Network_Node* node;
    
    //for testing
    RdmaResource(int t_partition,int t_threads,int current,char *_buffer,uint64_t _size,uint64_t _slotsize,uint64_t _off = 0);
    
    void Connect();
    void Servicing();
    
    //0 on success,-1 otherwise
    int RdmaRead(int t_id,int m_id,char *local,uint64_t size,uint64_t remote_offset);
    int RdmaWrite(int t_id,int m_id,char *local,uint64_t size,uint64_t remote_offset);
    int RdmaCmpSwap(int t_id,int m_id,char*local,uint64_t compare,uint64_t swap,uint64_t size,uint64_t off);
    int post(int t_id,int machine_id,char* local,uint64_t size,uint64_t remote_offset,ibv_wr_opcode op);
    int poll(int t_id,int machine_id);

    //TODO what if batched?    
    inline char *GetMsgAddr(int t_id) {
      return (char *)( buffer + off + t_id * slotsize);
    }
    static void* RecvThread(void * arg);



    inline int global_tid(int mid,int tid){
      return mid*_total_threads+tid;
    }
    
    struct RemoteQueueMeta{ //used to send message to remote queue
      uint64_t remote_tail; // directly write to remote_tail of remote machine
      pthread_spinlock_t remote_lock;
      char padding1[64];
      RemoteQueueMeta(){
        remote_tail=0;
        pthread_spin_init(&remote_lock,0);
      }
      void lock(){
        pthread_spin_lock(&remote_lock);
      }
      bool trylock(){
        return pthread_spin_trylock(&remote_lock);
      }
      void unlock(){
        pthread_spin_unlock(&remote_lock);
      }
    };
    struct LocalQueueMeta{
      uint64_t local_tail; // recv from here
      pthread_spinlock_t local_lock;
      char padding1[64];
      LocalQueueMeta(){
        local_tail=0;
        pthread_spin_init(&local_lock,0);
      }
      void lock(){
        pthread_spin_lock(&local_lock);
      }
      bool trylock(){
        return pthread_spin_trylock(&local_lock);
      }
      void unlock(){
        pthread_spin_unlock(&local_lock);
      }
    };
    std::vector<std::vector<RemoteQueueMeta> > RemoteMeta; //RemoteMeta[0..m-1][0..t-1]
    std::vector<std::vector< LocalQueueMeta> > LocalMeta;  //LocalMeta[0..t-1][0..m-1]
    uint64_t inline ceil(uint64_t original,uint64_t n){
      if(n==0){
        assert(false);
      }
      if(original%n == 0){
        return original;
      }
      return original - original%n +n; 
    }
    uint64_t start_of_recv_queue(int local_tid,int remote_mid){
      //[t0,m0][t0,m1] [t0,m5], [t1,m0],...
      uint64_t result=off+(_total_threads) * slotsize; //skip data-region and rdma_read-region
      result=result+rbf_size*(local_tid*_total_partition+remote_mid);
      return result;
    }

    
    void rbfSend(int local_tid,int remote_mid,int remote_tid,const char * str_ptr, uint64_t str_size){
      RemoteQueueMeta * meta=&RemoteMeta[remote_mid][remote_tid];
      meta->lock();
      uint64_t remote_start=start_of_recv_queue(remote_tid,_current_partition);
      if(_current_partition==remote_mid){
        char * ptr=buffer+remote_start;
        *((uint64_t*)(ptr+ (meta->remote_tail)%rbf_size )) = str_size;
        (meta->remote_tail)+=sizeof(uint64_t);
        for(uint64_t i=0;i<str_size;i++){
          *(ptr+(meta->remote_tail+i)%rbf_size)=str_ptr[i];
        }
        meta->remote_tail+=ceil(str_size,sizeof(uint64_t));
        *((uint64_t*)(ptr+(meta->remote_tail)%rbf_size))=str_size;
        (meta->remote_tail)+=sizeof(uint64_t);
      } else {
        uint64_t total_write_size=sizeof(uint64_t)*2+ceil(str_size,sizeof(uint64_t));
        char* local_buffer=GetMsgAddr(local_tid);
        *((uint64_t*)local_buffer)=str_size;
        local_buffer+=sizeof(uint64_t);
        memcpy(local_buffer,str_ptr,str_size);
        local_buffer+=ceil(str_size,sizeof(uint64_t));
        *((uint64_t*)local_buffer)=str_size;
        if(meta->remote_tail / rbf_size == (meta->remote_tail+total_write_size-1)/ rbf_size ){
          uint64_t remote_msg_offset=remote_start+(meta->remote_tail% rbf_size);
         RdmaWrite(local_tid,remote_mid,GetMsgAddr(local_tid),total_write_size,remote_msg_offset);
        } else {
          uint64_t length1=rbf_size - (meta->remote_tail % rbf_size);
          uint64_t length2=total_write_size-length1;
          uint64_t remote_msg_offset1=remote_start+(meta->remote_tail% rbf_size);
          uint64_t remote_msg_offset2=remote_start;
          RdmaWrite(local_tid,remote_mid,GetMsgAddr(local_tid),length1,remote_msg_offset1);
          RdmaWrite(local_tid,remote_mid,GetMsgAddr(local_tid)+length1,length2,remote_msg_offset2); 
        }
        meta->remote_tail =meta->remote_tail+total_write_size;        
      }
      meta->unlock();
    }
    bool check_rbf_msg(int local_tid,int mid){
      LocalQueueMeta * meta=&LocalMeta[local_tid][mid];
      char * rbf_ptr=buffer+start_of_recv_queue(local_tid,mid);
      uint64_t msg_size=*(volatile uint64_t*)(rbf_ptr+meta->local_tail%rbf_size );
      if(msg_size==0){
        return false;
      }
      return true;
    }
    std::string fetch_rbf_msg(int local_tid,int mid){
      LocalQueueMeta * meta=&LocalMeta[local_tid][mid];
      
      char * rbf_ptr=buffer+start_of_recv_queue(local_tid,mid);
      uint64_t msg_size=*(volatile uint64_t*)(rbf_ptr+meta->local_tail%rbf_size );
      
      //clear head
      *(uint64_t*)(rbf_ptr+(meta->local_tail)%rbf_size)=0;

      uint64_t skip_size=sizeof(uint64_t)+ceil(msg_size,sizeof(uint64_t));
      volatile uint64_t * msg_end_ptr=(uint64_t*)(rbf_ptr+ (meta->local_tail+skip_size)%rbf_size);
      while(*msg_end_ptr !=msg_size){
        uint64_t tmp=*msg_end_ptr;
        if(tmp!=0 && tmp!=msg_size){
          printf("waiting for %ld,but actually %ld\n",msg_size,tmp);
          exit(0);
        }
      }
      //clear tail
      *msg_end_ptr=0;

      std::string result;
      for(uint64_t i=0;i<ceil(msg_size,sizeof(uint64_t));i++){
        char * tmp=rbf_ptr+(meta->local_tail+sizeof(uint64_t)+i)%rbf_size;
        if(i<msg_size)
          result.push_back(*tmp);
        //clear data
        *tmp=0;
      }
      meta->local_tail+=2*sizeof(uint64_t)+ceil(msg_size,sizeof(uint64_t));
      return result;
    }
    std::string rbfRecv(int local_tid){
      while(true){
        for(int mid=0;mid<_total_partition;mid++){
          if(check_rbf_msg(local_tid,mid)){
            return fetch_rbf_msg(local_tid,mid);
          }
        }
      }
    }

    void set_need_help(int local_id,bool flag){
      local_meta[local_id].need_help=flag;
    }

    // bool check_rbf_msg(int local_tid,int mid,int tid,uint64_t& old_tail,uint64_t& msg_size){
    //   char * rbf_ptr=buffer+rbfOffset(_current_partition,local_tid,mid,tid);
    //   char * rbf_data_ptr=rbf_ptr+ sizeof(rbfMeta);
    //   uint64_t rbf_datasize=rbf_size-sizeof(rbfMeta);
    //   struct rbfMeta* meta=(rbfMeta*) rbf_ptr;
    //   msg_size=*(volatile uint64_t*)(rbf_data_ptr+meta->local_tail%rbf_datasize );
    //   if(msg_size==0){
    //     return false;
    //   }
    //   uint64_t padding=msg_size % sizeof(uint64_t);
    //   if(padding!=0)
    //     padding=sizeof(uint64_t)-padding;
    //   old_tail=meta->local_tail;
    //   meta->local_tail+=msg_size+padding+2*sizeof(uint64_t);
    //   return true;
    // }

    // struct rbfMeta{
    //   uint64_t local_tail; // used for polling
    //   uint64_t remote_tail; // directly write to remote_tail of remote machine
    //   uint64_t local_head; 
    //   char padding1[64-3*sizeof(uint64_t)];
    //   uint64_t copy_of_remote_head;
    //   char padding2[64-1*sizeof(uint64_t)];
    // };

    // uint64_t rbfOffset(int src_mid,int src_tid,int dst_mid,int dst_tid){
    //   uint64_t result=off+(_total_threads+src_tid) * slotsize;
    //   result=result+rbf_size*(dst_mid*_total_threads+dst_tid);
    //   return result;
    // }

    // void rbfSend(int local_tid,int remote_mid,int remote_tid,const char * str_ptr, uint64_t str_size){
    //   char * rbf_ptr=buffer+rbfOffset(_current_partition,local_tid,remote_mid,remote_tid);
    //   struct rbfMeta* meta=(rbfMeta*) rbf_ptr;
    //   uint64_t remote_rbf_offset=rbfOffset(remote_mid,remote_tid,_current_partition,local_tid);
    //   //TODO check whether we can send

    //   //Send data 
    //   uint64_t rbf_datasize=rbf_size-sizeof(rbfMeta);
    //   uint64_t padding=str_size % sizeof(uint64_t);
    //   if(padding!=0)
    //     padding=sizeof(uint64_t)-padding;
    //   uint64_t total_write_size=sizeof(uint64_t)*2+str_size+padding;

    //   if(_current_partition==remote_mid){
    //     // directly write
    //     char * ptr=buffer+remote_rbf_offset+sizeof(rbfMeta);
    //     *((uint64_t*)(ptr+(meta->remote_tail)%rbf_datasize))=str_size;
    //     (meta->remote_tail)+=sizeof(uint64_t);
    //     for(uint64_t i=0;i<str_size;i++){
    //       *(ptr+(meta->remote_tail)%rbf_datasize)=str_ptr[i];
    //       meta->remote_tail++;
    //     }
    //     meta->remote_tail+=padding;
    //     *((uint64_t*)(ptr+(meta->remote_tail)%rbf_datasize))=str_size;
    //     (meta->remote_tail)+=sizeof(uint64_t);
    //     //printf("tid=%d write to (%d,%d),tail=%ld\n",local_tid,remote_mid,remote_tid,meta->remote_tail);
    //   } else {
    //       char* local_buffer=GetMsgAddr(local_tid);
    //       *((uint64_t*)local_buffer)=str_size;
    //       local_buffer+=sizeof(uint64_t);
    //       memcpy(local_buffer,str_ptr,str_size);
    //       local_buffer+=str_size+padding;
    //       *((uint64_t*)local_buffer)=str_size;
    //       if(meta->remote_tail / rbf_datasize == (meta->remote_tail+total_write_size-1)/ rbf_datasize ){
    //         uint64_t remote_msg_offset=remote_rbf_offset+sizeof(rbfMeta)+(meta->remote_tail% rbf_datasize);
    //         RdmaWrite(local_tid,remote_mid,GetMsgAddr(local_tid),total_write_size,remote_msg_offset);
    //       } else {
    //         // we need to post 2 RdmaWrite
    //         uint64_t length1=rbf_datasize - (meta->remote_tail % rbf_datasize);
    //         uint64_t length2=total_write_size-length1;
    //         uint64_t remote_msg_offset1=remote_rbf_offset+sizeof(rbfMeta)+(meta->remote_tail% rbf_datasize);
    //         uint64_t remote_msg_offset2=remote_rbf_offset+sizeof(rbfMeta);
    //         RdmaWrite(local_tid,remote_mid,GetMsgAddr(local_tid),length1,remote_msg_offset1);
    //         RdmaWrite(local_tid,remote_mid,GetMsgAddr(local_tid)+length1,length2,remote_msg_offset2);  
    //       }
    //       meta->remote_tail =meta->remote_tail+total_write_size;
    //   }
    // }
    // bool check_rbf_msg(int local_tid,int mid,int tid,uint64_t& old_tail,uint64_t& msg_size){
    //   char * rbf_ptr=buffer+rbfOffset(_current_partition,local_tid,mid,tid);
    //   char * rbf_data_ptr=rbf_ptr+ sizeof(rbfMeta);
    //   uint64_t rbf_datasize=rbf_size-sizeof(rbfMeta);
    //   struct rbfMeta* meta=(rbfMeta*) rbf_ptr;
    //   msg_size=*(volatile uint64_t*)(rbf_data_ptr+meta->local_tail%rbf_datasize );
    //   if(msg_size==0){
    //     return false;
    //   }
    //   uint64_t padding=msg_size % sizeof(uint64_t);
    //   if(padding!=0)
    //     padding=sizeof(uint64_t)-padding;
    //   old_tail=meta->local_tail;
    //   meta->local_tail+=msg_size+padding+2*sizeof(uint64_t);
    //   return true;
    // }
    // std::string fetch_rbf_msg(int local_tid,int mid,int tid,uint64_t old_tail,uint64_t msg_size){
    //   char * rbf_ptr=buffer+rbfOffset(_current_partition,local_tid,mid,tid);
    //   char * rbf_data_ptr=rbf_ptr+ sizeof(rbfMeta);
    //   uint64_t rbf_datasize=rbf_size-sizeof(rbfMeta);
    //   struct rbfMeta* meta=(rbfMeta*) rbf_ptr;
    //   assert(msg_size!=0);
    //   *(uint64_t*)(rbf_data_ptr+old_tail%rbf_datasize)=0;
    //       //have message
    //   uint64_t padding=msg_size % sizeof(uint64_t);
    //   if(padding!=0)
    //     padding=sizeof(uint64_t)-padding;
    //   volatile uint64_t * msg_end_ptr=(uint64_t*)(rbf_data_ptr+ (old_tail+msg_size+padding+sizeof(uint64_t))%rbf_datasize);
    //   while(*msg_end_ptr !=msg_size){
    //     uint64_t tmp=*msg_end_ptr;
    //     if(tmp!=0 && tmp!=msg_size){
    //       printf("waiting for %ld,but actually %ld\n",msg_size,tmp);
    //       exit(0);
    //     }
    //   };
    //   *msg_end_ptr=0;
    //   std::string result;
    //   for(uint64_t i=0;i<msg_size+padding;i++){
    //     char * tmp=rbf_data_ptr+(old_tail+sizeof(uint64_t)+i)%rbf_datasize;
    //     if(i<msg_size)
    //       result.push_back(*tmp);
    //     *tmp=0;
    //   }
    //   return result;
    // }
    // void set_need_help(int local_id,bool flag){
    //   local_meta[local_id].need_help=flag;
    // }

    // bool tryRecv(int thread_id,int recver_id,std::string& ret_str){

    //   int max_try;
    //   int mid;
    //   int tid;
    //   if(thread_id==recver_id){
    //     max_try=_total_partition*_total_threads;
    //   } else {
    //     max_try=_total_partition;
    //   }
    //   //local_meta[recver_id].lock();
    //   if(!local_meta[recver_id].trylock()){
    //     return false;
    //   }
    //   while(max_try>0){
    //     max_try--;
    //     if(thread_id==recver_id){ //recv from my own queue
    //       //order (m0,t0),(m1,t0)
    //       mid=local_meta[thread_id].own_count % _total_partition;
    //       tid= (local_meta[thread_id].own_count / _total_partition)% _total_threads;
    //       local_meta[thread_id].own_count++;
    //     } else { //steal from someone's queue, only steal user request
    //       mid=local_meta[thread_id].steal_count[recver_id]% _total_partition;
    //       local_meta[thread_id].steal_count[recver_id]++;
    //       tid= 0 ;
    //     }
    //     uint64_t old_tail;
    //     uint64_t msg_size;
    //     if(!check_rbf_msg(recver_id,mid,tid,old_tail,msg_size)){
    //       continue;
    //     }
    //     local_meta[recver_id].unlock();
    //     ret_str=fetch_rbf_msg(recver_id,mid,tid,old_tail,msg_size);
    //     return true;
    //   }
    //   local_meta[recver_id].unlock();
    //   return false;
    // }
    // std::string rbfRecv(int local_tid){
    //     // prev_recv_mid is used to implement round-robin polling
    //   std::string ret_str;
    //   if(local_meta[local_tid].need_help){
    //     //Too busy to help other thread
    //     while(true){
    //       if(tryRecv(local_tid,local_tid,ret_str)){
    //         //local_meta[local_tid].recv_count++;
    //         return ret_str;
    //       }
    //       //local_meta[local_tid].unlock();
    //     }
    //   } else {
    //     while(true){

    //       int choice[2];
    //       choice[0]=local_tid;
    //       choice[1]=local_tid;
    //       if(local_tid>4){
    //         choice[1]-=4;
    //       }
    //       int t=choice[local_meta[local_tid].recv_count %2 ];
    //       //int t=1+ local_meta[local_tid].recv_count %(_total_threads-1);
          

    //       local_meta[local_tid].recv_count++;
    //       int tid;
    //       if(local_meta[t].need_help){
    //         tid=t;
    //       } else {
    //         tid=local_tid;
    //       }
    //       bool success=tryRecv(local_tid,tid,ret_str);
    //       if(success){
    //         return ret_str;
    //       }
    //     }
    //   }
    // }
};


#endif


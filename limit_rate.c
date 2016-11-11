#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/socket.h>
#include <net/sock.h>
#include <linux/sched.h>
#include <linux/dcache.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <net/inet_connection_sock.h>
#include <linux/ip.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <linux/spinlock_types.h>
#include <linux/rcupdate.h>

#include <linux/slab.h>


#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");

#define RATE_TIMER_FRE       10
#define RATE_TIMER_INTERVAL (HZ/RATE_TIMER_FRE)
//50hz 
static struct timer_list my_rate_timer;

static struct nf_hook_ops in_hook_ops;
static struct nf_hook_ops out_hook_ops;

#define SPEED_TABLE_MAX    200
#define SPEED_SOCK_MAX   64

#define SP_TBL_END    (-1)
#define SP_TBL_EMPTY  (SPEED_TABLE_MAX)
#define SP_TBL_NEW    (1<<1)
#define SP_TBL_SPEED_NO_LIMIT  (-1)

struct sock_pcb_node {
	struct task_struct * ppcb;
	struct sock * psock[SPEED_SOCK_MAX];
	unsigned int sock_count;
	int speed_limit_in;
	int speed_limit_out;
	int speed_in;
	int speed_out;
	int pack_count_in;
	int pack_count_out;
	unsigned int flag;
	int next;
};

struct sock_pcb_node speed_table[SPEED_TABLE_MAX];

static spinlock_t sp_tbl_lock = SPIN_LOCK_UNLOCKED;
//链表头
int sp_table_start = SP_TBL_END;  

extern rwlock_t tasklist_lock;


#define empty_table(i)  do{\
							speed_table[i].next = SP_TBL_EMPTY;\
							speed_table[i].sock_count = 0;\
							speed_table[i].speed_limit_in = SP_TBL_SPEED_NO_LIMIT;\
							speed_table[i].speed_limit_out = SP_TBL_SPEED_NO_LIMIT;\
							speed_table[i].speed_in = 0;\
							speed_table[i].speed_out = 0;\
							speed_table[i].pack_count_in = 0;\
							speed_table[i].pack_count_out = 0;\
							}while(0)
							
static void print_table(void)
{
	int i = 0;
	spin_lock(&sp_tbl_lock);
	i = sp_table_start;
	while(i!=SP_TBL_END)
	{
		printk("pid:%d sock_count:%d  index:%d \n",speed_table[i].ppcb->pid,speed_table[i].sock_count,i);
		i = speed_table[i].next;
	}
	spin_unlock(&sp_tbl_lock);
}


//用PCB 找速度表项
static int sp_table_find_by_pcb(struct task_struct *pcb)
{
	int sp_table_next = sp_table_start ;
	printk("find_pcb..start");
	while(sp_table_next != SP_TBL_END)
	{
		if(speed_table[sp_table_next].ppcb == pcb)
		{
			
			return sp_table_next;
		}
		sp_table_next = speed_table[sp_table_next].next;
	//	printk("next: %d",sp_table_next);
	}
	printk("end\n\r\n");
	return sp_table_next;
	
}

//找空项
static int sp_table_find_empty(void)
{
	static int i = 0;
	int the_first_loop = 1;
	while(speed_table[i].next != SP_TBL_EMPTY && i<=(SPEED_TABLE_MAX-1))
	{
		i++;
		if(i>=(SPEED_TABLE_MAX-1) && the_first_loop)
		{
			i = 0;
			the_first_loop = 0;
		}
	}
	return i;
}



//给出pcb 以添加新的表项,返回PCB在表中的位置
static int sp_table_add_pcb(struct task_struct *ppcb)
{
	int pcb_index = -1;
	int i = 0,j=0;
	struct file * myfile_fd = NULL;
	struct inode * the_inode  = NULL;
	struct fdtable *fdt = NULL;
	struct socket *the_socket = NULL;
	struct sock *the_sock = NULL;
	int sock_index = 0;
	unsigned long set;
	//寻找PCB
	printk("add_pcb_1\n");
	pcb_index = sp_table_find_by_pcb(ppcb);
	printk("add_pcb_2\n");
	if(pcb_index == SP_TBL_END)
	{
		//向头插入新结点
		int new_index = sp_table_find_empty();
		
		speed_table[new_index].next = sp_table_start;
		sp_table_start = new_index;
		pcb_index = new_index;
		speed_table[new_index].ppcb = ppcb;	
	}
	printk("add_pcb_3\n");
	fdt = files_fdtable(ppcb->files);
	if(fdt == NULL) 
	{
		printk("fdt NULL!\n");
		return pcb_index;
	}
	i = 0;
	j = 0;
	sock_index = 0;
	rcu_read_lock();
	
	for(;;)
	{
		
		i = j*__NFDBITS;
		if(i>= fdt->max_fdset || i>= fdt->max_fds)
		{
			break;
		}
		set = fdt->open_fds->fds_bits[j++];
		while(set!=0)
		{
			if(set&1)
			{
				myfile_fd = fdt->fd[i];
				if(myfile_fd != NULL)
				{
					the_inode = myfile_fd->f_dentry->d_inode;
					if(the_inode != NULL)
					if(S_ISSOCK(the_inode->i_mode))
					{
						the_socket = SOCKET_I(the_inode);
						if(the_socket == NULL || the_socket->sk ==NULL )
						{
							i++,set >>= 1;
							printk("sock NULL!\n");
							continue;
						}
						printk("find a sock\n");
						the_sock = the_socket->sk;
						speed_table[pcb_index].psock[sock_index] = the_sock;
						sock_index ++;
						if(sock_index >= SPEED_SOCK_MAX )
						{
							printk("sock full!!\n");
							sock_index--;
						}
					}
				}
			}
			i++,set >>= 1;
		}
	}
	rcu_read_unlock();
	printk("add_pcb_4\n");
	speed_table[pcb_index].sock_count = sock_index;
	return pcb_index;
}

static void flush_table(void)
{
	struct task_struct *ppcb = NULL;
//	struct list_head *cur_head = NULL;
	int pcb_index = 0;
	int *p_pre = (int *)NULL;
	int p_cur = -1;
	printk("flush_table_1\n");
//	cur_head = &(current->tasks);
//	read_lock(&tasklist_lock);
	spin_lock(&sp_tbl_lock);
	rcu_read_lock();
	
	for_each_process(ppcb)
	{
		if(ppcb == NULL) 
		{
			printk("ppcb NULL\n");
			continue;
		}
	//	printk("reading pid:%d\n",ppcb->pid);
		pcb_index = sp_table_add_pcb(ppcb);
		speed_table[pcb_index].flag |= SP_TBL_NEW; 
		speed_table[pcb_index].speed_out = speed_table[pcb_index].pack_count_out*RATE_TIMER_FRE;
		speed_table[pcb_index].pack_count_out = 0;
		speed_table[pcb_index].speed_in = speed_table[pcb_index].pack_count_in*RATE_TIMER_FRE;
		speed_table[pcb_index].pack_count_in =0;
	}
	rcu_read_unlock();
	printk("flush_table_2\n");
//	read_unlock(&tasklist_lock);
	//遍历链表以删除已退出程序的结点
	p_pre = &sp_table_start;
	p_cur = sp_table_start;
	while(p_cur != SP_TBL_END)
	{
		if((speed_table[p_cur].flag &SP_TBL_NEW) && speed_table[p_cur].sock_count != 0)
		{
			speed_table[p_cur].flag &= ~(SP_TBL_NEW);
			p_pre = &(speed_table[p_cur].next); 
			p_cur = speed_table[p_cur].next;
			
		}
		else
		{
		//如果此次未找到PCB 删除结点
			*p_pre = speed_table[p_cur].next;
			empty_table(p_cur);
//			printk("clean node ,index:%d\n",p_cur);
			p_cur = *p_pre;	
		}
	}
	spin_unlock(&sp_tbl_lock);
	printk("flush table\n\n\n\n\n");
//	print_table();
}


static int sp_tb_find_sock(struct sock* sock)
{
	int p_cur = -1;
	int i = 0;
	spin_lock(&sp_tbl_lock);
	p_cur = sp_table_start;
	while(p_cur != SP_TBL_END)
	{
		for(i=0;i<speed_table[p_cur].sock_count;i++)
		{
			if(sock == speed_table[p_cur].psock[i])
			{
				spin_unlock(&sp_tbl_lock);
				return p_cur;
			}
		}
		p_cur = speed_table[p_cur].next;
		
	}
	spin_unlock(&sp_tbl_lock);
	return SP_TBL_END;
}



static void timer_proc(unsigned long data)
{
	flush_table();
	my_rate_timer.expires = jiffies + RATE_TIMER_INTERVAL;
	add_timer(&my_rate_timer);
}

static unsigned int out_hook_fun(unsigned int hooknum,
								    struct sk_buff **skb,
								    const struct net_device *in,
								    const struct net_device *out,
								    int (*okfn) (struct sk_buff *))
{
	int index = 0;
	int sp_limit = 0;
	int sp_count = 0;
	int sp_pack_size = 0;
	index = sp_tb_find_sock((*skb)->sk);
	if(index==SP_TBL_END)
	{
		printk("hook sock not find\n");
		return NF_ACCEPT;
	}
	spin_lock(&sp_tbl_lock);
	sp_limit = speed_table[index].speed_limit_out;
	sp_count = speed_table[index].pack_count_out;
	
	sp_pack_size = (*skb)->len;
	if( ((sp_limit != SP_TBL_SPEED_NO_LIMIT)) && (sp_limit < sp_pack_size + sp_count) )
	{
		spin_unlock(&sp_tbl_lock);
		printk("out a pack ,from %d ,and drop\n",index);
		return NF_DROP;
	}
	speed_table[index].pack_count_out += sp_pack_size;
	spin_unlock(&sp_tbl_lock);
	printk("out a pack ,from %d ,and accpect\n",index);
	return NF_ACCEPT;
}


static unsigned int in_hook_fun(unsigned int hooknum,
								   struct sk_buff **skb,
								   const struct net_device *in,
								   const struct net_device *out,
								   int (*okfn)(struct sk_buff*))
{
	int index = 0;
	int sp_limit = 0;
	int sp_count = 0;
	int sp_pack_size = 0;
	index = sp_tb_find_sock((*skb)->sk);
	if(index==SP_TBL_END)
	{
		printk("hook sock not find\n");
		return NF_ACCEPT;
	}
	spin_lock(&sp_tbl_lock);
	sp_limit = speed_table[index].speed_limit_in;
	sp_count = speed_table[index].pack_count_in;
	
	sp_pack_size = (*skb)->len;
	if( ((sp_limit != SP_TBL_SPEED_NO_LIMIT)) && (sp_limit < sp_pack_size + sp_count) )
	{
		spin_unlock(&sp_tbl_lock);
		printk("in a pack ,to %d ,and drop\n",index);
		return NF_DROP;
	}
	speed_table[index].pack_count_in += sp_pack_size;
	spin_unlock(&sp_tbl_lock);
	printk("in a pack ,to %d ,and accpect\n",index);
	return NF_ACCEPT;
}

	

static int __init limit_rate_init(void)
{
	int i =0;
	printk("here!\n");
	//speed_table = kmalloc(sizeof(struct sock_pcb_node)*SPEED_TABLE_MAX,GFP_KERNEL );
	for(i=0; i< SPEED_TABLE_MAX ;i++)
	{
		empty_table(i);
	}
	printk("flush table finish \n");
	flush_table();
	
	init_timer(&my_rate_timer);
	my_rate_timer.expires = jiffies + RATE_TIMER_INTERVAL;
	my_rate_timer.function = timer_proc;
//	add_timer(&my_rate_timer);

	in_hook_ops.hook = in_hook_fun;
	in_hook_ops.hooknum = NF_IP_LOCAL_IN;
	in_hook_ops.pf = PF_INET;
	in_hook_ops.priority = NF_IP_PRI_FIRST;
//	nf_register_hook(&in_hook_ops);

	out_hook_ops.hook = out_hook_fun;
	out_hook_ops.hooknum = NF_IP_LOCAL_OUT;
	out_hook_ops.pf = PF_INET;
	out_hook_ops.priority = NF_IP_PRI_FIRST;
//	nf_register_hook(&out_hook_ops);
	return 0;
}

static void __exit limit_rate_exit(void)
{
	
	del_timer_sync(&my_rate_timer);
//	nf_unregister_hook(&out_hook_ops);
//	nf_unregister_hook(&in_hook_ops);
}



module_init(limit_rate_init);
module_exit(limit_rate_exit);






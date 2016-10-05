/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

/*-----------------------------------------------------------------------------
 * Pubsub low level API
 *----------------------------------------------------------------------------*/
void freePubsubPattern(void *p) /* 释放发布订阅的模式 */
int listMatchPubsubPattern(void *a, void *b) /* 发布订阅模式是否匹配 */
int clientSubscriptionsCount(redisClient *c) /* 返回客户端的所订阅的数量，包括channels + patterns管道和模式 */
int pubsubSubscribeChannel(redisClient *c, robj *channel) /* Client订阅一个Channel管道 */
int pubsubUnsubscribeChannel(redisClient *c, robj *channel, int notify) /* 取消订阅Client中的Channel */
int pubsubSubscribePattern(redisClient *c, robj *pattern) /* Client客户端订阅一种模式 */
int pubsubUnsubscribePattern(redisClient *c, robj *pattern, int notify) /* Client客户端取消订阅pattern模式 */
int pubsubUnsubscribeAllChannels(redisClient *c, int notify) /* 客户端取消自身订阅的所有Channel */
int pubsubUnsubscribeAllPatterns(redisClient *c, int notify) /* 客户端取消订阅所有的pattern模式 */
int pubsubPublishMessage(robj *channel, robj *message) /* 为所有订阅了Channel的Client发送消息message */

/* ------------PUB/SUB API ---------------- */
void subscribeCommand(redisClient *c) /* 订阅Channel的命令 */
void unsubscribeCommand(redisClient *c) /* 取消订阅Channel的命令 */
void psubscribeCommand(redisClient *c) /* 订阅模式命令 */
void punsubscribeCommand(redisClient *c) /* 取消订阅模式命令 */
void publishCommand(redisClient *c) /* 发布消息命令 */
void pubsubCommand(redisClient *c) /* 发布订阅命令 */
/*
 * 释放给定的模式 p
 */
void freePubsubPattern(void *p) {
    pubsubPattern *pat = p;

    decrRefCount(pat->pattern);
    zfree(pat);
}

/*
 * 对比模式 a 和 b 是否相同，相同返回 1 ，不相同返回 0 。
 */
int listMatchPubsubPattern(void *a, void *b) {
    pubsubPattern *pa = a, *pb = b;

    return (pa->client == pb->client) &&
           (equalStringObjects(pa->pattern,pb->pattern));
}

/* Subscribe a client to a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was already subscribed to that channel. 
 *
 * 设置客户端 c 订阅频道 channel （非模式pattern）。
 *
 * 订阅成功返回 1 ，如果客户端已经订阅了该频道，那么返回 0 。
 */
int pubsubSubscribeChannel(redisClient *c, robj *channel) {
    dictEntry *de;
    list *clients = NULL;
    int retval = 0;

    // redisClient.pubsub_channels 中保存客户端订阅的所有频道，以具体的频道名为key，
    // 而value为NULL。因此使用该字典能够快速判断客户端是否订阅了某频道。
    // 
    // server.pubsub_channels 中保存所有的频道和每个频道的订阅客户端，以具体的频
    // 道名为key，而value是一个列表，该列表中记录了订阅该频道的所有客户端。当向
    // 某频道发布消息时，就是通过查询该字典，将消息发送给订阅该频道的所有客户端。


    /* Add the channel to the client -> channels hash table */
    // 将 channels 频道添加到 redisClient->pubsub_channels 的集合中（值为 NULL 的字典视为集合）
    if (dictAdd(c->pubsub_channels,channel,NULL) == DICT_OK) {
        retval = 1;
        incrRefCount(channel);

        // 在服务器server维护的channel->clients 哈希表server.pubsub_channels中寻找指定的频道channel,
        // 来获取到 保存着所有订阅 channel 频道的客户端client的链表，如果不存在订阅channel的client链
        // 表，就创建一个空链表并将当前客户端添加进去。
        // 
        // server.pubsub_channels的关联示意图：
        // {
        //  频道名        订阅频道的客户端
        //  'channel-a' : [c1, c2, c3],
        //  'channel-b' : [c5, c2, c1],
        //  'channel-c' : [c10, c2, c1]
        // }
        de = dictFind(server.pubsub_channels,channel);
        if (de == NULL) {
            clients = listCreate();
            dictAdd(server.pubsub_channels,channel,clients);
            incrRefCount(channel);
        } else {
            clients = dictGetVal(de);
        }

        // 将当前客户端 c 添加到channel链表的尾部：
        // before:
        //   'channel' : [c1, c2]
        // after:
        //   'channel' : [c1, c2, c]
        listAddNodeTail(clients,c);
    }


    /* Notify the client */
    // 通知客户端。
    // 示例：
    // redis 127.0.0.1:6379> SUBSCRIBE xxx
    // Reading messages... (press Ctrl-C to quit)
    // 1) "subscribe"
    // 2) "xxx"
    // 3) (integer) 1
    // 
    addReply(c,shared.mbulkhdr[3]);     //输出"*3\r\n"
    addReply(c,shared.subscribebulk);   //输出“$9\r\nsubscribe\r\n”
    addReplyBulk(c,channel);            //将频道名称封装成"$4\r\ntest\r\n"形式
    // 客户端订阅的频道和模式总数
    addReplyLongLong(c, dictSize(c->pubsub_channels) + listLength(c->pubsub_patterns));

    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. 
 *
 * 客户端 c 退订频道 channel 。
 *
 * 如果取消成功返回 1 ，如果因为客户端未订阅频道，而造成取消失败，返回 0 。
 */
int pubsubUnsubscribeChannel(redisClient *c, robj *channel, int notify) {
    dictEntry *de;
    list *clients;
    listNode *ln;
    int retval = 0;

    /* Remove the channel from the client -> channels hash table */
    // 将频道 channel 从 client->channels 字典中移除
    incrRefCount(channel); /* channel may be just a pointer to the same object
                            we have in the hash tables. Protect it... */
    // 示意图：
    // before:
    // {
    //  'channel-x': NULL,
    //  'channel-y': NULL,
    //  'channel-z': NULL,
    // }
    // after unsubscribe channel-y ：
    // {
    //  'channel-x': NULL,
    //  'channel-z': NULL,
    // }
    

    // 将频道 channel 从 client->pubsub_channels 字典中移除
    if (dictDelete(c->pubsub_channels,channel) == DICT_OK) {

        // channel 移除成功，表示客户端订阅了这个频道，执行以下代码

        retval = 1;
        /* Remove the client from the channel -> clients list hash table */
        // 从server.pubsub_channels获取channel->clients 链表，从中移除 client.
        // 示意图：
        // before:
        // {
        //  'channel-x' : [c1, c2, c3],
        // }
        // after c2 unsubscribe channel-x:
        // {
        //  'channel-x' : [c1, c3]
        // }
        de = dictFind(server.pubsub_channels,channel);  //根据channel从dict定位到list
        redisAssertWithInfo(c,NULL,de != NULL);         //判断de不空
        clients = dictGetVal(de);                       //获取该list
        ln = listSearchKey(clients,c);                  //从list定位到client node
        redisAssertWithInfo(c,NULL,ln != NULL);         //判断client node不空
        listDelNode(clients,ln);                        //从链表中删除该client node


        // 如果移除 client 之后链表为空，那么删除这个 channel 键。
        // 示意图：
        // 
        // before
        // {
        //  'channel-x' : [c1]
        // }
        // 
        // after c1 ubsubscribe channel-x
        // {
        //  'channel-x' : []
        // }
        // 
        // then also delete 'channel-x' key in dict
        // {
        //  // nothing here
        // }
        // 
        if (listLength(clients) == 0) {
            /* Free the list and associated hash entry at all if this was
             * the latest client, so that it will be possible to abuse
             * Redis PUBSUB creating millions of channels. */
            dictDelete(server.pubsub_channels,channel);
        }
    }

    /* Notify the client */
    // 回复客户端
    if (notify) {

        addReply(c,shared.mbulkhdr[3]);     //输出"*3\r\n"
        addReply(c,shared.unsubscribebulk); //输出“$9\r\nsubscribe\r\n”
        addReplyBulk(c,channel);            //将频道名称封装成"$4\r\ntest\r\n"形式
        // 退订频道之后客户端仍在订阅的频道和模式的总数
        addReplyLongLong(c, dictSize(c->pubsub_channels) + listLength(c->pubsub_patterns));

    }

    decrRefCount(channel); /* it is finally safe to release it */

    return retval;
}

/* Subscribe a client to a pattern. Returns 1 if the operation succeeded, or 0 if the client was already subscribed to that pattern. 
 *
 * 设置客户端 c 订阅模式 pattern 频道。
 *
 * 订阅成功返回 1 ，如果客户端已经订阅了该模式，那么返回 0 。
 */
int pubsubSubscribePattern(redisClient *c, robj *pattern) {
    int retval = 0;


    // redisClient.pubsub_patterns 中保存客户端订阅的所有模式频道，可以查看
    // 客户端订阅了多少频道以及客户端是否订阅某个频道
    // 
    // server.pubsub_patterns 中保存所有的模式频道和每个模式频道的订阅客户端
    // ，可以将消息发布到订阅客户端



    // 在链表中查找模式pattern，看客户端是否已经订阅了这个模式。
    // 这里为什么不像 channel 那样，用字典来进行检测呢？
    // 因为pattern时需要逐个检测是否匹配的，用list比较合适。
    if (listSearchKey(c->pubsub_patterns,pattern) == NULL) {
        
        //如果Client->pubsub_patterns中没有订阅pattern，则进行添加。

        retval = 1;
        pubsubPattern *pat;

        // 将 pattern 添加到 c->pubsub_patterns 链表中。
        listAddNodeTail(c->pubsub_patterns,pattern);
        incrRefCount(pattern);
        // 创建并设置新的 pubsubPattern 结构
        pat = zmalloc(sizeof(*pat));
        pat->pattern = getDecodedObject(pattern);
        pat->client  = c;
        // 添加到server.pubsub_patterns链表末尾
        listAddNodeTail(server.pubsub_patterns,pat);
    }


    /* Notify the client */
    // 回复客户端。
    // 示例：
    // redis 127.0.0.1:6379> PSUBSCRIBE xxx*
    // Reading messages... (press Ctrl-C to quit)
    // 1) "psubscribe"
    // 2) "xxx*"
    // 3) (integer) 1
    
    addReply(c,shared.mbulkhdr[3]);
    addReply(c,shared.psubscribebulk);
    addReplyBulk(c,pattern);
    addReplyLongLong(c, dictSize(c->pubsub_channels) + listLength(c->pubsub_patterns));

    return retval;
}

/* Unsubscribe a client from a channel. Returns 1 if the operation succeeded, or
 * 0 if the client was not subscribed to the specified channel. 
 *
 * 取消客户端 c 对模式 pattern 的订阅。
 *
 * 取消成功返回 1 ，因为客户端未订阅 pattern 而造成取消失败，返回 0 。
 */
int pubsubUnsubscribePattern(redisClient *c, robj *pattern, int notify) {
    listNode *ln;
    pubsubPattern pat;
    int retval = 0;

    incrRefCount(pattern); /* Protect the object. May be the same we remove */

    // 先确认一下，客户端是否订阅了这个模式
    if ((ln = listSearchKey(c->pubsub_patterns,pattern)) != NULL) {

        retval = 1;

        // 将模式从客户端的订阅列表中删除
        listDelNode(c->pubsub_patterns,ln);

        // 设置 pubsubPattern 结构
        pat.client = c;
        pat.pattern = pattern;

        // 在服务器中查找
        ln = listSearchKey(server.pubsub_patterns,&pat);
        listDelNode(server.pubsub_patterns,ln);
    }

    /* Notify the client */
    // 回复客户端
    if (notify) {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.punsubscribebulk);
        addReplyBulk(c,pattern);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));
    }

    decrRefCount(pattern);

    return retval;
}

/* Unsubscribe from all the channels. Return the number of channels the
 * client was subscribed from. 
 *
 * 退订客户端 c 订阅的所有频道。
 *
 * 返回被退订频道的总数。
 */
int pubsubUnsubscribeAllChannels(redisClient *c, int notify) {

    // 频道迭代器
    dictIterator *di = dictGetSafeIterator(c->pubsub_channels);
    dictEntry *de;
    int count = 0;

    // 退订
    while((de = dictNext(di)) != NULL) {
        robj *channel = dictGetKey(de);
        count += pubsubUnsubscribeChannel(c,channel,notify);
    }

    /* We were subscribed to nothing? Still reply to the client. */
    // 如果在执行这个函数时，客户端没有订阅任何频道，
    // 那么向客户端发送回复
    if (notify && count == 0) {
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.unsubscribebulk);
        addReply(c,shared.nullbulk);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+listLength(c->pubsub_patterns));
    }

    dictReleaseIterator(di);

    // 被退订的频道的数量
    return count;
}

/* Unsubscribe from all the patterns. Return the number of patterns the
 * client was subscribed from. 
 *
 * 退订客户端 c 订阅的所有模式。
 *
 * 返回被退订模式的数量。
 */
int pubsubUnsubscribeAllPatterns(redisClient *c, int notify) {
    listNode *ln;
    listIter li;
    int count = 0;

    // 迭代客户端订阅模式的链表
    listRewind(c->pubsub_patterns,&li);
    while ((ln = listNext(&li)) != NULL) {
        robj *pattern = ln->value;

        // 退订，并计算退订数
        count += pubsubUnsubscribePattern(c,pattern,notify);
    }

    // 如果在执行这个函数时，客户端没有订阅任何模式，
    // 那么向客户端发送回复
    if (notify && count == 0) {
        /* We were subscribed to nothing? Still reply to the client. */
        addReply(c,shared.mbulkhdr[3]);
        addReply(c,shared.punsubscribebulk);
        addReply(c,shared.nullbulk);
        addReplyLongLong(c,dictSize(c->pubsub_channels)+ listLength(c->pubsub_patterns));
    }

    // 退订总数
    return count;
}

/* Publish a message 
 *
 *使用 PUBLISH 命令向订阅者发送消息，需要执行以下两个步骤： 　　
 *  1) 使用给定的频道作为键，在 redisServer.pubsub_channels 字典中查找记录了订阅
 *      这个频道的所有客户端clients的链表，遍历这个链表，将消息发布给所有订阅者。
 *       　　
 *  2) 遍历 redisServer.pubsub_patterns 链表，将链表中的模式和给定的频道进行匹配，
 *      如果匹配成功，那么将消息发布到相应模式的客户端当中。
 */
int pubsubPublishMessage(robj *channel, robj *message) {
    int receivers = 0;
    dictEntry *de;
    listNode *ln;
    listIter li;

    /* Send to clients listening for that channel */
    // 取出频道 channel 的订阅客户端的链表，并将消息发送给其中clients。
    de = dictFind(server.pubsub_channels,channel);
    if (de) {
        list *list = dictGetVal(de);
        listNode *ln;
        listIter li;

        // 遍历客户端链表，将 message 发送给它们
        listRewind(list,&li);
        while ((ln = listNext(&li)) != NULL) {
            redisClient *c = ln->value;

            // 回复客户端。
            // 示例：
            //   1) "message"
            //   2) "test"
            //   3) "hello"
            addReply(c,shared.mbulkhdr[3]); //输出"$5\r\nhello\r\n"
            addReply(c,shared.messagebulk); //输出"$7\r\nmessage\r\n"
            addReplyBulk(c,channel);        //输出"$4\r\ntest\r\n"
            addReplyBulk(c,message);        //输出"$5\r\nhello\r\n"

            // 接收客户端计数
            receivers++;
        }
    }

    /* Send to clients listening to matching channels */
    // 将消息也发送给那些和频道匹配的模式
    if (listLength(server.pubsub_patterns)) {

        // 遍历模式链表
        listRewind(server.pubsub_patterns,&li);
        channel = getDecodedObject(channel);
        while ((ln = listNext(&li)) != NULL) {

            // 取出 pubsubPattern
            pubsubPattern *pat = ln->value;

            // 如果 channel 和 pattern 匹配
            // 就给所有订阅该 pattern 的客户端发送消息
            if (stringmatchlen((char*)pat->pattern->ptr, sdslen(pat->pattern->ptr),
                               (char*)channel->ptr, sdslen(channel->ptr),0)) {

                // 回复客户端
                // 示例：
                // 1) "pmessage"
                // 2) "*"
                // 3) "xxx"
                // 4) "hello"
                addReply(pat->client,shared.mbulkhdr[4]);
                addReply(pat->client,shared.pmessagebulk);
                addReplyBulk(pat->client,pat->pattern);     //打印匹配的pattern
                addReplyBulk(pat->client,channel);          //打印频道号
                addReplyBulk(pat->client,message);          //打印消息
                receivers++;                                //对接收消息的客户端进行+1计数
            }
        }

        decrRefCount(channel);  //释放用过的channle
    }

    // 返回接收的client计数
    return receivers;
}

/*-----------------------------------------------------------------------------
 * Pubsub commands implementation
 *----------------------------------------------------------------------------*/

void subscribeCommand(redisClient *c) {
    int j;

    for (j = 1; j < c->argc; j++)
        pubsubSubscribeChannel(c,c->argv[j]);
}

void unsubscribeCommand(redisClient *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllChannels(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribeChannel(c,c->argv[j],1);
    }
}

//当客户端发来PSUBSCRIBE命令之后，函数psubscribeCommand对命令参数中每个频道模式pattern
//进行订阅。通过调用函数pubsubSubscribePattern，修改server和client中相应的数据结构，完
//成订阅操作。最后，向客户端标志位中增加REDIS_PUBSUB标记，表示该客户端进入订阅模式。
void psubscribeCommand(redisClient *c) {
    int j;
    for (j = 1; j < c->argc; j++)
        pubsubSubscribePattern(c,c->argv[j]);
    c->flags |= REDIS_PUBSUB;  
}

void punsubscribeCommand(redisClient *c) {
    if (c->argc == 1) {
        pubsubUnsubscribeAllPatterns(c,1);
    } else {
        int j;

        for (j = 1; j < c->argc; j++)
            pubsubUnsubscribePattern(c,c->argv[j],1);
    }
}

//当客户端向Redis服务器发送”PUBLISH <channel>  <message>”命令后，
//Redis服务器会将消息<message>发送给所有订阅了<channel>的客户端，
//以及那些订阅了与<channel>相匹配的频道模式的客户端。
//
//
//函数中，首先调用pubsubPublishMessage函数，将message发布到相应的频道；
//
//然后，如果当前Redis处于集群模式下，则调用clusterPropagatePublish函数，向所有集群节点广播该消息；
//否则，调用forceCommandPropagation函数，向客户端c的标志位中增加REDIS_FORCE_REPL标记，以便后续能
//将该PUBLISH命令传递给从节点；
//
//最后，将接收消息的客户端个数回复给客户端c；
//
void publishCommand(redisClient *c) {

    //c->argv[1]为频道的名称，c->argv[2]为消息对象。下面函数就是将消息发送给所有订
    //阅频道的客户端，返回订阅该频道的客户端总数。
    int receivers = pubsubPublishMessage(c->argv[1],c->argv[2]); 

    if (server.cluster_enabled)
        clusterPropagatePublish(c->argv[1],c->argv[2]);
    else
        forceCommandPropagation(c,REDIS_PROPAGATE_REPL);
    
    //给调用publish命令的客户端发送订阅c->argv[1]频道的客户端数。
    addReplyLongLong(c,receivers);
}


/* PUBSUB command for Pub/Sub introspection. */
/* 发布订阅命令 */
void pubsubCommand(redisClient *c) {

    // PUBSUB CHANNELS [pattern] 子命令
    if (!strcasecmp(c->argv[1]->ptr,"channels") && (c->argc == 2 || c->argc ==3))
    {
        /* PUBSUB CHANNELS [<pattern>] */
        // 检查命令请求是否给定了 pattern 参数
        // 如果没有给定的话，就设为 NULL
        sds pat = (c->argc == 2) ? NULL : c->argv[2]->ptr;

        // 创建 pubsub_channels 的字典迭代器
        // 该字典的键为频道，值为链表
        // 链表中保存了所有订阅键所对应的频道的客户端
        dictIterator *di = dictGetIterator(server.pubsub_channels);
        dictEntry *de;
        long mblen = 0;
        void *replylen;

        replylen = addDeferredMultiBulkLength(c);
        // 从迭代器中获取一个客户端
        while((de = dictNext(di)) != NULL) {

            // 从字典中取出客户端所订阅的频道
            robj *cobj = dictGetKey(de);
            sds channel = cobj->ptr;

            // 顺带一提
            // 因为 Redis 的字典实现只能遍历字典的值（客户端）
            // 所以这里才会有遍历字典值然后通过字典值取出字典键（频道）的蹩脚用法

            // 如果没有给定 pattern 参数，那么打印所有找到的频道
            // 如果给定了 pattern 参数，那么只打印和 pattern 相匹配的频道
            if (!pat || stringmatchlen(pat, sdslen(pat),
                                       channel, sdslen(channel),0))
            {
                // 向客户端输出频道
                addReplyBulk(c,cobj);
                mblen++;
            }
        }
        // 释放字典迭代器
        dictReleaseIterator(di);
        setDeferredMultiBulkLength(c,replylen,mblen);

    // PUBSUB NUMSUB [channel-1 channel-2 ... channel-N] 子命令
    } else if (!strcasecmp(c->argv[1]->ptr,"numsub") && c->argc >= 2) {
        /* PUBSUB NUMSUB [Channel_1 ... Channel_N] */
        int j;

        addReplyMultiBulkLen(c,(c->argc-2)*2);
        for (j = 2; j < c->argc; j++) {

            // c->argv[j] 也即是客户端输入的第 N 个频道名字
            // pubsub_channels 的字典为频道名字
            // 而值则是保存了 c->argv[j] 频道所有订阅者的链表
            // 而调用 dictFetchValue 也就是取出所有订阅给定频道的客户端
            list *l = dictFetchValue(server.pubsub_channels,c->argv[j]);

            addReplyBulk(c,c->argv[j]);
            // 向客户端返回链表的长度属性
            // 这个属性就是某个频道的订阅者数量
            // 例如：如果一个频道有三个订阅者，那么链表的长度就是 3
            // 而返回给客户端的数字也是三
            addReplyBulkLongLong(c,l ? listLength(l) : 0);
        }

    // PUBSUB NUMPAT 子命令
    } else if (!strcasecmp(c->argv[1]->ptr,"numpat") && c->argc == 2) {
        /* PUBSUB NUMPAT */

        // pubsub_patterns 链表保存了服务器中所有被订阅的模式
        // pubsub_patterns 的长度就是服务器中被订阅模式的数量
        addReplyLongLong(c,listLength(server.pubsub_patterns));

    // 错误处理
    } else {
        addReplyErrorFormat(c,
            "Unknown PUBSUB subcommand or wrong number of arguments for '%s'",
            (char*)c->argv[1]->ptr);
    }
}

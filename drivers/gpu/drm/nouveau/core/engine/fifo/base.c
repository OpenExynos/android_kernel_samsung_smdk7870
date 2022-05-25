/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */

#include <core/client.h>
#include <core/object.h>
#include <core/handle.h>
#include <core/event.h>
#include <nvif/unpack.h>
#include <nvif/class.h>
#include <nvif/event.h>

#include <engine/dmaobj.h>
#include <engine/fifo.h>

static int
nouveau_fifo_event_ctor(struct nouveau_object *object, void *data, u32 size,
			struct nvkm_notify *notify)
{
	if (size == 0) {
		notify->size  = 0;
		notify->types = 1;
		notify->index = 0;
		return 0;
	}
	return -ENOSYS;
}

static const struct nvkm_event_func
nouveau_fifo_event_func = {
	.ctor = nouveau_fifo_event_ctor,
};

int
nouveau_fifo_channel_create_(struct nouveau_object *parent,
			     struct nouveau_object *engine,
			     struct nouveau_oclass *oclass,
			     int bar, u32 addr, u32 size, u32 pushbuf,
			     u64 engmask, int len, void **ptr)
{
	struct nouveau_device *device = nv_device(engine);
	struct nouveau_fifo *priv = (void *)engine;
	struct nouveau_fifo_chan *chan;
	struct nouveau_dmaeng *dmaeng;
	unsigned long flags;
	int ret;

	/* create base object class */
	ret = nouveau_namedb_create_(parent, engine, oclass, 0, NULL,
				     engmask, len, ptr);
	chan = *ptr;
	if (ret)
		return ret;

	/* validate dma object representing push buffer */
	chan->pushdma = (void *)nouveau_handle_ref(parent, pushbuf);
	if (!chan->pushdma)
		return -ENOENT;

	dmaeng = (void *)chan->pushdma->base.engine;
	switch (chan->pushdma->base.oclass->handle) {
	case NV_DMA_FROM_MEMORY:
	case NV_DMA_IN_MEMORY:
		break;
	default:
		return -EINVAL;
	}

	ret = dmaeng->bind(chan->pushdma, parent, &chan->pushgpu);
	if (ret)
		return ret;

	/* find a free fifo channel */
	spin_lock_irqsave(&priv->lock, flags);
	for (chan->chid = priv->min; chan->chid < priv->max; chan->chid++) {
		if (!priv->channel[chan->chid]) {
			priv->channel[chan->chid] = nv_object(chan);
			break;
		}
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	if (chan->chid == priv->max) {
		nv_error(priv, "no free channels\n");
		return -ENOSPC;
	}

	chan->addr = nv_device_resource_start(device, bar) +
		     addr + size * chan->chid;
	chan->size = size;
	nvkm_event_send(&priv->cevent, 1, 0, NULL, 0);
	return 0;
}

void
nouveau_fifo_channel_destroy(struct nouveau_fifo_chan *chan)
{
	struct nouveau_fifo *priv = (void *)nv_object(chan)->engine;
	unsigned long flags;

	if (chan->user)
		iounmap(chan->user);

	spin_lock_irqsave(&priv->lock, flags);
	priv->channel[chan->chid] = NULL;
	spin_unlock_irqrestore(&priv->lock, flags);

	nouveau_gpuobj_ref(NULL, &chan->pushgpu);
	nouveau_object_ref(NULL, (struct nouveau_object **)&chan->pushdma);
	nouveau_namedb_destroy(&chan->base);
}

void
_nouveau_fifo_channel_dtor(struct nouveau_object *object)
{
	struct nouveau_fifo_chan *chan = (void *)object;
	nouveau_fifo_channel_destroy(chan);
}

int
_nouveau_fifo_channel_map(struct nouveau_object *object, u64 *addr, u32 *size)
{
	struct nouveau_fifo_chan *chan = (void *)object;
	*addr = chan->addr;
	*size = chan->size;
	return 0;
}

u32
_nouveau_fifo_channel_rd32(struct nouveau_object *object, u64 addr)
{
	struct nouveau_fifo_chan *chan = (void *)object;
	if (unlikely(!chan->user)) {
		chan->user = ioremap(chan->addr, chan->size);
		if (WARN_ON_ONCE(chan->user == NULL))
			return 0;
	}
	return ioread32_native(chan->user + addr);
}

void
_nouveau_fifo_channel_wr32(struct nouveau_object *object, u64 addr, u32 data)
{
	struct nouveau_fifo_chan *chan = (void *)object;
	if (unlikely(!chan->user)) {
		chan->user = ioremap(chan->addr, chan->size);
		if (WARN_ON_ONCE(chan->user == NULL))
			return;
	}
	iowrite32_native(data, chan->user + addr);
}

int
nouveau_fifo_uevent_ctor(struct nouveau_object *object, void *data, u32 size,
			 struct nvkm_notify *notify)
{
	union {
		struct nvif_notify_uevent_req none;
	} *req = data;
	int ret;

	if (nvif_unvers(req->none)) {
		notify->size  = sizeof(struct nvif_notify_uevent_rep);
		notify->types = 1;
		notify->index = 0;
	}

	return ret;
}

void
nouveau_fifo_uevent(struct nouveau_fifo *fifo)
{
	struct nvif_notify_uevent_rep rep = {
	};
	nvkm_event_send(&fifo->uevent, 1, 0, &rep, sizeof(rep));
}

int
_nouveau_fifo_channel_ntfy(struct nouveau_object *object, u32 type,
			   struct nvkm_event **event)
{
	struct nouveau_fifo *fifo = (void *)object->engine;
	switch (type) {
	case G82_CHANNEL_DMA_V0_NTFY_UEVENT:
		if (nv_mclass(object) >= G82_CHANNEL_DMA) {
			*event = &fifo->uevent;
			return 0;
		}
		break;
	default:
		break;
	}
	return -EINVAL;
}

static int
nouveau_fifo_chid(struct nouveau_fifo *priv, struct nouveau_object *object)
{
	int engidx = nv_hclass(priv) & 0xff;

	while (object && object->parent) {
		if ( nv_iclass(object->parent, NV_ENGCTX_CLASS) &&
		    (nv_hclass(object->parent) & 0xff) == engidx)
			return nouveau_fifo_chan(object)->chid;
		object = object->parent;
	}

	return -1;
}

const char *
nouveau_client_name_for_fifo_chid(struct nouveau_fifo *fifo, u32 chid)
{
	struct nouveau_fifo_chan *chan = NULL;
	unsigned long flags;

	spin_lock_irqsave(&fifo->lock, flags);
	if (chid >= fifo->min && chid <= fifo->max)
		chan = (void *)fifo->channel[chid];
	spin_unlock_irqrestore(&fifo->lock, flags);

	return nouveau_client_name(chan);
}

void
nouveau_fifo_destroy(struct nouveau_fifo *priv)
{
	kfree(priv->channel);
	nvkm_event_fini(&priv->uevent);
	nvkm_event_fini(&priv->cevent);
	nouveau_engine_destroy(&priv->base);
}

int
nouveau_fifo_create_(struct nouveau_object *parent,
		     struct nouveau_object *engine,
		     struct nouveau_oclass *oclass,
		     int min, int max, int length, void **pobject)
{
	struct nouveau_fifo *priv;
	int ret;

	ret = nouveau_engine_create_(parent, engine, oclass, true, "PFIFO",
				     "fifo", length, pobject);
	priv = *pobject;
	if (ret)
		return ret;

	priv->min = min;
	priv->max = max;
	priv->channel = kzalloc(sizeof(*priv->channel) * (max + 1), GFP_KERNEL);
	if (!priv->channel)
		return -ENOMEM;

	ret = nvkm_event_init(&nouveau_fifo_event_func, 1, 1, &priv->cevent);
	if (ret)
		return ret;

	priv->chid = nouveau_fifo_chid;
	spin_lock_init(&priv->lock);
	return 0;
}

/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Collection of classes for doing specific steps from the main program.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#ifndef __CL_ENABLE_EXCEPTIONS
# define __CL_ENABLE_EXCEPTIONS
#endif

#include <cstddef>
#include <vector>
#include <CL/cl.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/thread/locks.hpp>
#include <boost/ref.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include "grid.h"
#include "workers.h"
#include "work_queue.h"
#include "splat_tree_cl.h"
#include "splat.h"
#include "splat_set.h"
#include "bucket.h"
#include "mesh.h"
#include "mesh_filter.h"
#include "statistics.h"
#include "statistics_cl.h"
#include "errors.h"
#include "thread_name.h"
#include "misc.h"

MesherGroupBase::Worker::Worker(MesherGroup &owner)
    : WorkerBase("mesher", 0), owner(owner) {}

void MesherGroupBase::Worker::operator()(WorkItem &item)
{
    Timeplot::Action timer("compute", getTimeplotWorker(), owner.getComputeStat());
    owner.input(item.work, getTimeplotWorker());
    owner.meshBuffer.free(item.alloc);
}

MesherGroup::MesherGroup(std::size_t memMesh)
    : WorkerGroup<MesherGroupBase::WorkItem, MesherGroupBase::Worker, MesherGroup>(
        "mesher", 1),
    meshBuffer("mem.MesherGroup.mesh", memMesh)
{
    addWorker(new Worker(*this));
}

boost::shared_ptr<MesherGroup::WorkItem> MesherGroup::get(Timeplot::Worker &tworker, std::size_t size)
{
    boost::shared_ptr<WorkItem> item = WorkerGroup<WorkItem, Worker, MesherGroup>::get(tworker, size);
    std::size_t rounded = roundUp(size, sizeof(cl_ulong)); // to ensure alignment
    item->alloc = meshBuffer.allocate(tworker, rounded, &getStat);
    return item;
}


DeviceWorkerGroup::DeviceWorkerGroup(
    std::size_t numWorkers, std::size_t spare,
    OutputGenerator outputGenerator,
    const cl::Context &context, const cl::Device &device,
    std::size_t maxBucketSplats, Grid::size_type maxCells,
    std::size_t meshMemory,
    int levels, int subsampling, float boundaryLimit,
    MlsShape shape)
:
    Base("device", numWorkers),
    progress(NULL), outputGenerator(outputGenerator),
    context(context), device(device),
    maxBucketSplats(maxBucketSplats), maxCells(maxCells), meshMemory(meshMemory),
    subsampling(subsampling),
    copyQueue(context, device, CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE),
    itemPool(),
    popMutex(NULL),
    popCondition(NULL)
{
    for (std::size_t i = 0; i < numWorkers; i++)
    {
        addWorker(new Worker(*this, context, device, levels, boundaryLimit, shape, i));
    }
    const std::size_t items = numWorkers + spare;
    const std::size_t maxItemSplats = maxBucketSplats; // the same thing for now
    for (std::size_t i = 0; i < items; i++)
    {
        boost::shared_ptr<WorkItem> item = boost::make_shared<WorkItem>(context, maxItemSplats);
        itemPool.push(item);
    }
    unallocated_ = maxItemSplats * items;

    CLH::ResourceUsage usage = resourceUsage(
        numWorkers, spare, device,
        maxBucketSplats, maxCells, meshMemory, levels);
    usage.addStatistics(Statistics::Registry::getInstance(), "mem.device.");
}

void DeviceWorkerGroup::start(const Grid &fullGrid)
{
    this->fullGrid = fullGrid;
    Base::start();
}

bool DeviceWorkerGroup::canGet()
{
    return !itemPool.empty();
}

boost::shared_ptr<DeviceWorkerGroup::WorkItem> DeviceWorkerGroup::get(
    Timeplot::Worker &tworker, std::size_t numSplats)
{
    Timeplot::Action timer("get", tworker, getStat);
    timer.setValue(numSplats * sizeof(Splat));
    boost::shared_ptr<DeviceWorkerGroup::WorkItem> item = itemPool.pop();

    boost::lock_guard<boost::mutex> unallocatedLock(unallocatedMutex);
    unallocated_ -= numSplats;
    return item;
}

void DeviceWorkerGroup::freeItem(boost::shared_ptr<WorkItem> item)
{
    item->subItems.clear();
    item->copyEvent = cl::Event(); // release the reference

    if (popCondition != NULL)
    {
        boost::lock_guard<boost::mutex> popLock(*popMutex);
        itemPool.push(item);
        popCondition->notify_one();
    }
    else
        itemPool.push(item);
}

std::size_t DeviceWorkerGroup::unallocated()
{
    boost::lock_guard<boost::mutex> unallocatedLock(unallocatedMutex);
    return unallocated_;
}

Grid::size_type DeviceWorkerGroupBase::computeMaxSwathe(
    Grid::size_type yMax,
    Grid::size_type y,
    Grid::size_type yAlign,
    Grid::size_type zAlign)
{
    y = roundUp(y, yAlign);
    if (yMax < y)
        return zAlign; // no even enough space for a single slice
    Grid::size_type chunks = (yMax - y) / (y * zAlign);
    if (chunks == 0)
        chunks = 1;
    return chunks * zAlign;
}

CLH::ResourceUsage DeviceWorkerGroup::resourceUsage(
    std::size_t numWorkers, std::size_t spare,
    const cl::Device &device,
    std::size_t maxBucketSplats, Grid::size_type maxCells,
    std::size_t meshMemory,
    int levels)
{
    Grid::size_type block = maxCells + 1;
    Grid::size_type maxSwathe = computeMaxSwathe(
        MAX_IMAGE_HEIGHT, block, MlsFunctor::wgs[1], MlsFunctor::wgs[2]);

    CLH::ResourceUsage workerUsage;
    workerUsage += Marching::resourceUsage(
        device, block, block, block,
        maxSwathe, meshMemory, MlsFunctor::wgs);
    workerUsage += SplatTreeCL::resourceUsage(device, levels, maxBucketSplats);

    const std::size_t maxItemSplats = maxBucketSplats; // the same thing for now
    CLH::ResourceUsage itemUsage;
    itemUsage.addBuffer("splats", maxItemSplats * sizeof(Splat));
    return workerUsage * numWorkers + itemUsage * (numWorkers + spare);
}

DeviceWorkerGroupBase::Worker::Worker(
    DeviceWorkerGroup &owner,
    const cl::Context &context, const cl::Device &device,
    int levels, float boundaryLimit,
    MlsShape shape, int idx)
:
    WorkerBase("device", idx),
    owner(owner),
    queue(context, device, Statistics::isEventTimingEnabled() ? CL_QUEUE_PROFILING_ENABLE : 0),
    tree(context, device, levels, owner.maxBucketSplats),
    input(context, shape),
    marching(context, device, owner.maxCells + 1, owner.maxCells + 1, owner.maxCells + 1,
             computeMaxSwathe(MAX_IMAGE_HEIGHT, owner.maxCells + 1, input.alignment()[1], input.alignment()[2]),
             owner.meshMemory, input.alignment()),
    scaleBias(context)
{
    input.setBoundaryLimit(boundaryLimit);
    filterChain.addFilter(boost::ref(scaleBias));
}

void DeviceWorkerGroupBase::Worker::start()
{
    scaleBias.setScaleBias(owner.fullGrid);
}

void DeviceWorkerGroupBase::Worker::operator()(WorkItem &work)
{
    Timeplot::Action timer("compute", getTimeplotWorker(), owner.getComputeStat());
    BOOST_FOREACH(const SubItem &sub, work.subItems)
    {
        cl_uint3 keyOffset;
        for (int i = 0; i < 3; i++)
            keyOffset.s[i] = sub.grid.getExtent(i).first;
        // same thing, just as a different type for a different API
        Grid::difference_type offset[3] =
        {
            (Grid::difference_type) keyOffset.s[0],
            (Grid::difference_type) keyOffset.s[1],
            (Grid::difference_type) keyOffset.s[2]
        };

        Grid::size_type size[3];
        for (int i = 0; i < 3; i++)
        {
            /* Note: numVertices not numCells, because Marching does per-vertex queries.
             * So we need information about the cell that is just beyond the last vertex,
             * just to avoid special-casing it.
             */
            size[i] = sub.grid.numVertices(i);
        }

        /* We need to round up the octree size to a multiple of the granularity used for MLS. */
        Grid::size_type expandedSize[3];
        for (int i = 0; i < 3; i++)
            expandedSize[i] = roundUp(size[i], MlsFunctor::wgs[i]);

        filterChain.setOutput(owner.outputGenerator(sub.chunkId, getTimeplotWorker()));

        cl::Event treeBuildEvent;
        std::vector<cl::Event> wait(1);

        wait[0] = work.copyEvent;
        tree.enqueueBuild(queue, work.splats, sub.firstSplat, sub.numSplats,
                          expandedSize, offset, owner.subsampling, &wait, &treeBuildEvent);
        wait[0] = treeBuildEvent;

        input.set(offset, tree, owner.subsampling);
        marching.generate(queue, input, filterChain, size, keyOffset, &wait);

        tree.clearSplats();

        if (owner.progress != NULL)
            *owner.progress += sub.progressSplats;

        {
            boost::lock_guard<boost::mutex> unallocatedLock(owner.unallocatedMutex);
            owner.unallocated_ += sub.numSplats;
        }
    }
}

CopyGroup::CopyGroup(
    const std::vector<DeviceWorkerGroup *> &outGroups,
    std::size_t maxQueueSplats)
:
    WorkerGroup<CopyGroup::WorkItem, CopyGroup::Worker, CopyGroup>(
        "copy", 1),
    outGroups(outGroups),
    maxDeviceItemSplats(outGroups[0]->getMaxItemSplats()),
    splatBuffer("mem.CopyGroup.splats", maxQueueSplats * sizeof(Splat)),
    writeStat(Statistics::getStatistic<Statistics::Variable>("copy.write")),
    splatsStat(Statistics::getStatistic<Statistics::Variable>("copy.splats")),
    sizeStat(Statistics::getStatistic<Statistics::Variable>("copy.size"))
{
    addWorker(new Worker(*this, outGroups[0]->getContext(), outGroups[0]->getDevice()));
    BOOST_FOREACH(DeviceWorkerGroup *g, outGroups)
        g->setPopCondition(&popMutex, &popCondition);
}

CopyGroupBase::Worker::Worker(
    CopyGroup &owner, const cl::Context &context, const cl::Device &device)
    : WorkerBase("copy", 0), owner(owner),
    pinned("mem.CopyGroup.pinned", context, device, owner.maxDeviceItemSplats),
    bufferedItems("mem.CopyGroup.bufferedItems"),
    bufferedSplats(0)
{
}

void CopyGroupBase::Worker::flush()
{
    if (bufferedItems.empty())
        return;

    boost::unique_lock<boost::mutex> popLock(owner.popMutex);
    DeviceWorkerGroup *outGroup = NULL;
    while (true)
    {
        /* Try all devices for which we can pop immediately, and take the one that
         * seems likely to run out the soonest. It's a poor guess, but does at
         * least make sure that we always service totally idle devices before ones
         * that still have work queued.
         */
        std::size_t best = 0;
        BOOST_FOREACH(DeviceWorkerGroup *g, owner.outGroups)
        {
            if (g->canGet())
            {
                std::size_t u = g->unallocated();
                if (u >= best)
                {
                    best = u;
                    outGroup = g;
                }
            }
        }
        if (outGroup != NULL)
            break;

        // No spare slots. Wait until there is one
        {
            Timeplot::Action timer("get", getTimeplotWorker(), owner.outGroups[0]->getGetStat());
            owner.popCondition.wait(popLock);
        }
    }
    popLock.release()->unlock();

    // This should now never block
    boost::shared_ptr<DeviceWorkerGroup::WorkItem> item = outGroup->get(getTimeplotWorker(), bufferedSplats);
    item->subItems.swap(bufferedItems);
    outGroup->getCopyQueue().enqueueWriteBuffer(
        item->splats,
        CL_FALSE,
        0, bufferedSplats * sizeof(Splat),
        pinned.get(),
        NULL, &item->copyEvent);
    cl::Event copyEvent = item->copyEvent;
    outGroup->push(getTimeplotWorker(), item);

    /* Ensures that we can start refilling the pinned memory right away. Note
     * that this is not the same as doing a synchronous transfer, because we
     * are still overlapping the transfer with enqueuing the item.
     */
    {
        Timeplot::Action writeTimer("write", getTimeplotWorker(), owner.getWriteStat());
        writeTimer.setValue(bufferedSplats * sizeof(Splat));
        copyEvent.wait();
    }
    bufferedSplats = 0;
}

void CopyGroupBase::Worker::operator()(WorkItem &work)
{
    Timeplot::Action timer("compute", getTimeplotWorker(), owner.getComputeStat());
    timer.setValue(work.numSplats * sizeof(Splat));

    if (bufferedSplats + work.numSplats > owner.maxDeviceItemSplats)
        flush();

    const Splat *in = work.getSplats();
    Splat *out = pinned.get() + bufferedSplats;
    std::size_t progressSplats = 0;
    for (std::size_t i = 0; i < work.numSplats; i++)
    {
        /* Each splat is accounted in the progress meter with the
         * bin it is inside (half-open intervals). Note that this
         * test is a short-cut that makes assumptions about the
         * grid written by BucketLoader.
         */
        bool inside = true;
        for (int j = 0; j < 3; j++)
        {
            Grid::extent_type e = work.grid.getExtent(j);
            float p = in[i].position[j];
            inside = inside && p >= e.first && p < e.second;
        }
        progressSplats += inside;
        out[i] = in[i];
    }
    DeviceWorkerGroup::SubItem subItem;
    subItem.chunkId = work.chunkId;
    subItem.grid = work.grid;
    subItem.numSplats = work.numSplats;
    subItem.firstSplat = bufferedSplats;
    subItem.progressSplats = progressSplats;
    bufferedItems.push_back(subItem);
    bufferedSplats += work.numSplats;

    owner.splatsStat.add(work.numSplats);
    owner.sizeStat.add(work.grid.numCells());

    owner.splatBuffer.free(work.splats);
}

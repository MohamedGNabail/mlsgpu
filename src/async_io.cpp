/**
 * Asynchronous writes through a @ref BinaryWriter.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <cstddef>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>
#include "binary_io.h"
#include "work_queue.h"
#include "worker_group.h"
#include "circular_buffer.h"
#include "timeplot.h"
#include "async_io.h"

namespace detail
{

void AsyncWriterWorker::operator()(AsyncWriterItem &item)
{
    Timeplot::Action timer("write", getTimeplotWorker(), owner.getComputeStat());
    item.out->write(item.get(), item.count, item.offset);
}

AsyncWriterWorker::AsyncWriterWorker(AsyncWriter &owner)
    : WorkerBase("asyncwriter", 0), owner(owner)
{
}

} // namespace detail

boost::shared_ptr<AsyncWriterItem> AsyncWriter::get(
    Timeplot::Worker &tworker, std::size_t bytes)
{
    boost::shared_ptr<AsyncWriterItem> item = Base::get(tworker, bytes);
    item->alloc = buffer.allocate(tworker, bytes, &getStat);
    item->count = bytes;
    return item;
}

void AsyncWriter::push(
    Timeplot::Worker &tworker,
    boost::shared_ptr<AsyncWriterItem> item,
    boost::shared_ptr<BinaryWriter> out,
    std::size_t count,
    BinaryWriter::offset_type offset)
{
    MLSGPU_ASSERT(count <= item->count, std::invalid_argument);
    item->count = count;
    item->out = out;
    item->offset = offset;
    Base::push(tworker, item);
}

void AsyncWriter::freeItem(boost::shared_ptr<AsyncWriterItem> item)
{
    buffer.free(item->alloc);
    item->out.reset(); // to release the reference
}

AsyncWriter::AsyncWriter(std::size_t numWorkers, std::size_t bufferSize)
    : Base("asyncwriter", numWorkers),
    buffer("mem.asyncwriter.buffer", bufferSize)
{
    for (std::size_t i = 0; i < numWorkers; i++)
        addWorker(new detail::AsyncWriterWorker(*this));
}

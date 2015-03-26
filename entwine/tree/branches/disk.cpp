/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/tree/branches/disk.hpp>

#include <sys/mman.h>

#include <pdal/PointView.hpp>

#include <entwine/third/json/json.h>
#include <entwine/tree/roller.hpp>
#include <entwine/tree/point-info.hpp>
#include <entwine/types/linking-point-view.hpp>
#include <entwine/types/schema.hpp>
#include <entwine/types/simple-point-table.hpp>
#include <entwine/types/single-point-table.hpp>
#include <entwine/util/fs.hpp>
#include <entwine/util/platform.hpp>

namespace entwine
{

namespace
{
    std::size_t pointsPerChunk(std::size_t depthBegin, std::size_t dimensions)
    {
        return
            Branch::calcOffset(depthBegin + 1, dimensions) -
            Branch::calcOffset(depthBegin, dimensions);
    }

    std::size_t numChunks(
            std::size_t depthBegin,
            std::size_t depthEnd,
            std::size_t dimensions)
    {
        const std::size_t numPoints(
                Branch::calcOffset(depthEnd, dimensions) -
                Branch::calcOffset(depthBegin, dimensions));

        return numPoints / pointsPerChunk(depthBegin, dimensions);
    }

    std::vector<char> makeEmptyChunk(
            const Schema& schema,
            std::size_t pointsPerChunk)
    {
        SimplePointTable table(schema);
        pdal::PointView view(table);

        for (std::size_t i(0); i < pointsPerChunk; ++i)
        {
            view.setField(pdal::Dimension::Id::X, i, INFINITY);
            view.setField(pdal::Dimension::Id::Y, i, INFINITY);
        }

        return table.data();
    }

    std::string getFilename(const std::string& path, const std::size_t id)
    {
        return path + "/" + std::to_string(id);
    }

    const std::ios_base::openmode binaryTruncMode(
            std::ofstream::binary |
            std::ofstream::out |
            std::ofstream::trunc);
}

Chunk::Chunk(
        const std::string& path,
        std::size_t firstPointIndex,
        const std::vector<char>& initData)
    : m_fd()
    , m_mapping(0)
    , m_mutex()
{
    std::cout << "Making chunk" << std::endl;
    std::string filename(getFilename(path, firstPointIndex));

    if (!fs::fileExists(filename))
    {
        fs::writeFile(filename, initData, binaryTruncMode);
    }

    m_fd.reset(new fs::FileDescriptor(filename));

    if (!m_fd->good())
    {
        throw std::runtime_error("Could not open " + filename);
    }

    char* mapping(
            static_cast<char*>(mmap(
                0,
                initData.size(),
                PROT_READ | PROT_WRITE,
                MAP_SHARED,
                m_fd->id(),
                0)));

    if (mapping == MAP_FAILED)
        throw std::runtime_error("Could not map " + filename);

    m_mapping = mapping;
}

bool Chunk::addPoint(
        const Schema& schema,
        const Roller& roller,
        PointInfo** toAddPtr,
        const std::size_t byteOffset)
{
    bool done(false);
    PointInfo* toAdd(*toAddPtr);

    char* pos(m_mapping + byteOffset);

    SinglePointTable table(schema, pos);
    LinkingPointView view(table);

    // TODO Should have multiple for high-traffic chunks close to the base.
    std::lock_guard<std::mutex> lock(m_mutex);

    double x = view.getFieldAs<double>(pdal::Dimension::Id::X, 0);
    double y = view.getFieldAs<double>(pdal::Dimension::Id::Y, 0);

    if (Point::exists(x, y))
    {
        const Point curPoint(x, y);
        const Point mid(roller.bbox().mid());

        if (toAdd->point->sqDist(mid) < curPoint.sqDist(mid))
        {
            // Pull out the old stored value.
            PointInfo* old(
                    new PointInfo(
                        new Point(curPoint),
                        pos,
                        schema.pointSize()));

            toAdd->write(pos);
            delete toAdd->point;
            delete toAdd;

            *toAddPtr = old;
        }
    }
    else
    {
        // Empty point here - store incoming.
        toAdd->write(pos);

        delete toAdd->point;
        delete toAdd;
        done = true;
    }

    return done;
}

LockedChunk::LockedChunk()
    : m_mutex()
    , m_chunk(0)
{ }

LockedChunk::~LockedChunk()
{
    if (exists())
    {
        delete m_chunk.load();
    }
}

void LockedChunk::init(
        const std::string& path,
        const std::size_t firstPointIndex,
        const std::vector<char>& initData)
{
    if (!exists())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!exists())
        {
            m_chunk.store(new Chunk(path, firstPointIndex, initData));
        }
    }
}

bool LockedChunk::exists() const
{
    return m_chunk.load() != 0;
}

DiskBranch::DiskBranch(
        const std::string& path,
        const Schema& schema,
        const std::size_t dimensions,
        const std::size_t depthBegin,
        const std::size_t depthEnd)
    : Branch(schema, dimensions, depthBegin, depthEnd)
    , m_path(path)
    , m_pointsPerChunk(pointsPerChunk(depthBegin, dimensions))
    , m_chunks(numChunks(depthBegin, depthEnd, dimensions))
    , m_emptyChunk(makeEmptyChunk(schema, m_pointsPerChunk))
{ }

DiskBranch::DiskBranch(
        const std::string& path,
        const Schema& schema,
        const std::size_t dimensions,
        const Json::Value& meta)
    : Branch(schema, dimensions, meta)
    , m_path(path)
    , m_pointsPerChunk(pointsPerChunk(depthBegin(), dimensions))
    , m_chunks(numChunks(depthBegin(), depthEnd(), dimensions))
    , m_emptyChunk(makeEmptyChunk(schema, m_pointsPerChunk))
{ }

bool DiskBranch::addPoint(PointInfo** toAddPtr, const Roller& roller)
{
    // Zero-based index of this chunk.
    const std::size_t chunkIndex(getChunkId(roller.pos()));

    // First point position belonging to this chunk.
    const std::size_t chunkFirstPoint(
            indexBegin() + chunkIndex * m_pointsPerChunk);

    LockedChunk& lockedChunk(m_chunks[chunkIndex]);

    if (!lockedChunk.exists())
    {
        // TODO This is relatively infrequent, so could lock here and add this
        // chunkId to some structure for bookkeeping.

        lockedChunk.init(m_path, chunkFirstPoint, m_emptyChunk);
    }

    Chunk& chunk(lockedChunk.get());

    return chunk.addPoint(
            schema(),
            roller,
            toAddPtr,
            getByteOffset(chunkFirstPoint, roller.pos()));
}

bool DiskBranch::hasPoint(std::size_t index)
{
    bool result(false);

    /*
    if (m_chunks[getChunkId(index)].exists())
    {

    }
    */

    return result;
}

Point DiskBranch::getPoint(std::size_t index)
{
    return Point();
}

std::vector<char> DiskBranch::getPointData(std::size_t index)
{
    return std::vector<char>();
}

std::size_t DiskBranch::getChunkId(const std::size_t index) const
{
    assert(index >= indexBegin() && index < indexEnd());

    const std::size_t pointOffset(index - indexBegin());
    return pointOffset / m_pointsPerChunk;
}

std::size_t DiskBranch::getByteOffset(
        const std::size_t chunkFirstPoint,
        const std::size_t index) const
{
    assert(index >= chunkFirstPoint);

    return (index - chunkFirstPoint) * schema().pointSize();
}

void DiskBranch::saveImpl(const std::string& path, Json::Value& meta)
{
    // TODO
    /*
    meta["ids"]
    */
}

} // namespace entwine

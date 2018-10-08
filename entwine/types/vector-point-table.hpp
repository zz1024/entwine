/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

#include <pdal/PointRef.hpp>
#include <pdal/PointTable.hpp>

#include <entwine/types/schema.hpp>

namespace entwine
{

class MemBlock
{
public:
    using Block = std::vector<char>;

    MemBlock(uint64_t pointSize, uint64_t pointsPerBlock)
        : m_pointSize(pointSize)
        , m_pointsPerBlock(pointsPerBlock)
        , m_bytesPerBlock(m_pointsPerBlock * m_pointSize)
    {
        m_blocks.reserve(8);
        m_refs.reserve(m_pointsPerBlock);
    }

    char* next()
    {
        if (m_pos == m_end)
        {
            m_blocks.emplace_back(Block(m_bytesPerBlock));
            m_pos = m_blocks.back().data();
            m_end = m_pos + m_bytesPerBlock;
        }

        char* result(m_pos);
        m_refs.push_back(m_pos);
        m_pos += m_pointSize;
        return result;
    }

    uint64_t size() const { return m_refs.size(); }
    const std::vector<char*>& refs() const { return m_refs; }
    void clear()
    {
        m_blocks.clear();
        m_pos = nullptr;
        m_end = nullptr;
        m_refs.clear();
    }

private:
    const uint64_t m_pointSize;
    const uint64_t m_pointsPerBlock;
    const uint64_t m_bytesPerBlock;

    std::vector<Block> m_blocks;
    char* m_pos = nullptr;
    char* m_end = nullptr;

    std::vector<char*> m_refs;
};

// For writing.
class BlockPointTable : public pdal::SimplePointTable
{
public:
    BlockPointTable(const Schema& schema, MemBlock& a, MemBlock& b)
        : SimplePointTable(schema.pdalLayout())
    {
        m_refs.reserve(a.size() + b.size());
        m_refs.insert(m_refs.end(), a.refs().begin(), a.refs().end());
        m_refs.insert(m_refs.end(), b.refs().begin(), b.refs().end());
    }

    virtual char* getPoint(pdal::PointId index) override
    {
        return m_refs[index];
    }

    virtual pdal::PointId addPoint() override { return m_index++; }
    virtual bool supportsView() const override { return true; }
    uint64_t size() const { return m_refs.size(); }

private:
    std::vector<char*> m_refs;
    uint64_t m_index = 0;
};

// For reading.
class VectorPointTable : public pdal::StreamPointTable
{
    using Process = std::function<void()>;

public:
    VectorPointTable(const Schema& schema, std::size_t np = 4096)
        : pdal::StreamPointTable(schema.pdalLayout())
        , m_pointSize(schema.pointSize())
        , m_data(np * m_pointSize, 0)
        , m_size(np)
        , m_skips(m_size, false)
    { }

    VectorPointTable(const Schema& schema, std::vector<char>&& data)
        : pdal::StreamPointTable(schema.pdalLayout())
        , m_pointSize(schema.pointSize())
        , m_data(std::move(data))
        , m_size(m_data.size() / m_pointSize)
        , m_skips(m_size, false)
    {
        if (m_data.size() % m_pointSize != 0)
        {
            throw std::runtime_error("Invalid VectorPointTable data");
        }
    }

    std::size_t size() const { return m_size; }
    void resize(std::size_t np)
    {
        m_data.resize(np * m_pointSize, 0);
        m_size = np;
        m_skips.resize(m_size, false);
    }

    void assign(std::vector<char>&& data)
    {
        m_data = std::move(data);
        m_size = m_data.size() / m_pointSize;
        m_skips.resize(m_size, false);
    }

    pdal::point_count_t capacity() const override
    {
        return m_data.size() / m_pointSize;
    }

    pdal::PointRef at(pdal::PointId index)
    {
        if (index >= size())
        {
            throw std::out_of_range("Invalid index to VectorPointTable::at");
        }

        return pdal::PointRef(*this, index);
    }

    pdal::PointRef append()
    {
        return pdal::PointRef(*this, addPoint());
    }

    virtual char* getPoint(pdal::PointId index) override
    {
        return m_data.data() + index * m_pointSize;
    }

    std::vector<char>& data() { return m_data; }
    const std::vector<char>& data() const { return m_data; }

    std::vector<char>&& acquire() { return std::move(m_data); }

    void setProcess(Process f) { m_f = f; }
    void reset() override { m_f(); }

    class Iterator
    {
    public:
        explicit Iterator(VectorPointTable& table, pdal::PointId index = 0)
            : m_table(table)
            , m_index(index)
            , m_pointRef(m_table, m_index)
        {
            while (m_index < m_table.size() && m_table.skip(m_index))
            {
                ++m_index;
            }
            m_pointRef.setPointId(m_index);
        }

        Iterator& operator++()
        {
            do
            {
                m_pointRef.setPointId(++m_index);
            }
            while (m_index < m_table.size() && m_table.skip(m_index));

            return *this;
        }

        Iterator operator++(int)
        {
            Iterator other(*this);
            ++other;
            return other;
        }

        pdal::PointRef& operator*() { return m_pointRef; }
        pdal::PointRef& pointRef() { return m_pointRef; }
        bool operator!=(const Iterator& rhs) { return m_index != rhs.m_index; }
        char* data() { return m_table.getPoint(m_index); }

    private:
        VectorPointTable& m_table;
        pdal::PointId m_index;
        pdal::PointRef m_pointRef;
    };

    Iterator begin() { return Iterator(*this); }
    Iterator end() { return Iterator(*this, size()); }

    std::size_t pointSize() const { return m_pointSize; }
    virtual bool skip(pdal::PointId n) const override { return m_skips.at(n); }

private:
    virtual void setNumPoints(pdal::PointId s) override { m_size = s; }
    virtual void setSkip(pdal::PointId n) override { m_skips.at(n) = true; }

    VectorPointTable(const VectorPointTable&);
    VectorPointTable& operator=(const VectorPointTable&);

    const std::size_t m_pointSize;
    std::vector<char> m_data;
    std::size_t m_size = 0;
    std::vector<bool> m_skips;

    Process m_f = []() { };
};

} // namespace entwine


#include "NativeSplitter.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <Functions/FunctionFactory.h>
#include <Parser/SerializedPlanParser.h>
#include <Common/Exception.h>
#include <boost/asio/detail/eventfd_select_interrupter.hpp>
#include <jni/jni_common.h>
#include <Common/Exception.h>
#include <Common/JNIUtils.h>
#include <Core/Block.h>
#include <base/types.h>
#include <Poco/Logger.h>
#include <Common/logger_useful.h>
#include <Poco/StringTokenizer.h>
#include <string>

namespace local_engine
{
jclass NativeSplitter::iterator_class = nullptr;
jmethodID NativeSplitter::iterator_has_next = nullptr;
jmethodID NativeSplitter::iterator_next = nullptr;

void NativeSplitter::split(DB::Block & block)
{
    if (block.rows() == 0)
    {
        return;
    }
    if (!output_header.columns()) [[unlikely]]
    {
        if (output_columns_indicies.empty())
        {
            output_header = block.cloneEmpty();
            for (size_t i = 0; i < block.columns(); ++i)
            {
                output_columns_indicies.push_back(i);
            }
        }
        else
        {
            DB::ColumnsWithTypeAndName cols;
            for (const auto & index : output_columns_indicies)
            {
                cols.push_back(block.getByPosition(index));
            }
            output_header = DB::Block(cols);
        }
    }
    computePartitionId(block);
    DB::Block out_block;
    for (size_t col = 0; col < output_header.columns(); ++col)
    {
        out_block.insert(block.getByPosition(output_columns_indicies[col]));
    }
    for (size_t col = 0; col < output_header.columns(); ++col)
    {
        for (size_t j = 0; j < partition_info.partition_num; ++j)
        {
            size_t from = partition_info.partition_start_points[j];
            size_t length = partition_info.partition_start_points[j + 1] - from;
            if (length == 0)
                continue; // no data for this partition continue;
            partition_buffer[j]->appendSelective(col, out_block, partition_info.partition_selector, from, length);
        }
    }

    bool has_active_sender = false;
    for (size_t i = 0; i < options.partition_nums; ++i)
    {
        if (partition_buffer[i]->size() >= options.buffer_size)
        {
            output_buffer.emplace(std::pair(i, std::make_unique<Block>(partition_buffer[i]->releaseColumns())));
        }
    }
}

NativeSplitter::NativeSplitter(Options options_, jobject input_) : options(options_)
{
    GET_JNIENV(env)
    input = env->NewGlobalRef(input_);
    partition_buffer.reserve(options.partition_nums);
    for (size_t i = 0; i < options.partition_nums; ++i)
    {
        partition_buffer.emplace_back(std::make_shared<ColumnsBuffer>(options.buffer_size));
    }
    CLEAN_JNIENV
}

NativeSplitter::~NativeSplitter()
{
    GET_JNIENV(env)
    env->DeleteGlobalRef(input);
    CLEAN_JNIENV
}

bool NativeSplitter::hasNext()
{
    while (output_buffer.empty())
    {
        if (inputHasNext())
        {
            split(*reinterpret_cast<Block *>(inputNext()));
        }
        else
        {
            for (size_t i = 0; i < options.partition_nums; ++i)
            {
                auto buffer = partition_buffer.at(i);
                if (buffer->size() > 0)
                {
                    output_buffer.emplace(std::pair(i, new Block(buffer->releaseColumns())));
                }
            }
            break;
        }
    }
    if (!output_buffer.empty())
    {
        next_partition_id = output_buffer.top().first;
        setCurrentBlock(*output_buffer.top().second);
        produce();
    }
    return !output_buffer.empty();
}

DB::Block * NativeSplitter::next()
{
    if (!output_buffer.empty())
    {
        output_buffer.pop();
    }
    consume();
    return &currentBlock();
}

int32_t NativeSplitter::nextPartitionId()
{
    return next_partition_id;
}

bool NativeSplitter::inputHasNext()
{
    GET_JNIENV(env)
    bool next = safeCallBooleanMethod(env, input, iterator_has_next);
    CLEAN_JNIENV
    return next;
}

int64_t NativeSplitter::inputNext()
{
    GET_JNIENV(env)
    int64_t result = safeCallLongMethod(env, input, iterator_next);
    CLEAN_JNIENV
    return result;
}
std::unique_ptr<NativeSplitter> NativeSplitter::create(const std::string & short_name, Options options_, jobject input)
{
    if (short_name == "rr")
    {
        return std::make_unique<RoundRobinNativeSplitter>(options_, input);
    }
    else if (short_name == "hash")
    {
        return std::make_unique<HashNativeSplitter>(options_, input);
    }
    else if (short_name == "single")
    {
        options_.partition_nums = 1;
        return std::make_unique<RoundRobinNativeSplitter>(options_, input);
    }
    else if (short_name == "range")
    {
        return std::make_unique<RangePartitionNativeSplitter>(options_, input);
    }
    else
    {
        throw std::runtime_error("unsupported splitter " + short_name);
    }
}

HashNativeSplitter::HashNativeSplitter(NativeSplitter::Options options_, jobject input)
    : NativeSplitter(options_, input)
{
    Poco::StringTokenizer exprs_list(options_.exprs_buffer, ",");
    std::vector<size_t> hash_fields;
    for (auto iter = exprs_list.begin(); iter != exprs_list.end(); ++iter)
    {
        hash_fields.push_back(std::stoi(*iter));
    }

    Poco::StringTokenizer output_column_tokenizer(options_.schema_buffer, ",");
    for (auto iter = output_column_tokenizer.begin(); iter != output_column_tokenizer.end(); ++iter)
    {
        output_columns_indicies.push_back(std::stoi(*iter));
    }

    selector_builder = std::make_unique<HashSelectorBuilder>(options.partition_nums, hash_fields, "cityHash64");
}

void HashNativeSplitter::computePartitionId(Block & block)
{
    partition_info = selector_builder->build(block);
}

RoundRobinNativeSplitter::RoundRobinNativeSplitter(NativeSplitter::Options options_, jobject input) : NativeSplitter(options_, input)
{
    Poco::StringTokenizer output_column_tokenizer(options_.schema_buffer, ",");
    for (auto iter = output_column_tokenizer.begin(); iter != output_column_tokenizer.end(); ++iter)
    {
        output_columns_indicies.push_back(std::stoi(*iter));
    }
    selector_builder = std::make_unique<RoundRobinSelectorBuilder>(options_.partition_nums);
}

void RoundRobinNativeSplitter::computePartitionId(Block & block)
{
    partition_info = selector_builder->build(block);
}

RangePartitionNativeSplitter::RangePartitionNativeSplitter(NativeSplitter::Options options_, jobject input)
    : NativeSplitter(options_, input)
{
    Poco::StringTokenizer output_column_tokenizer(options_.schema_buffer, ",");
    for (auto iter = output_column_tokenizer.begin(); iter != output_column_tokenizer.end(); ++iter)
    {
        output_columns_indicies.push_back(std::stoi(*iter));
    }
    selector_builder = std::make_unique<RangeSelectorBuilder>(options_.exprs_buffer, options_.partition_nums);
}

void RangePartitionNativeSplitter::computePartitionId(DB::Block & block)
{
    partition_info = selector_builder->build(block);
}

}

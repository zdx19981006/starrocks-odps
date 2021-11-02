// This file is licensed under the Elastic License 2.0. Copyright 2021 StarRocks Limited.

#include "exec/vectorized/tablet_scanner.h"

#include <memory>
#include <utility>

#include "column/column_helper.h"
#include "column/column_pool.h"
#include "column/field.h"
#include "column/fixed_length_column.h"
#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "exec/vectorized/olap_scan_node.h"
#include "runtime/current_mem_tracker.h"
#include "storage/field.h"
#include "storage/storage_engine.h"
#include "storage/vectorized/chunk_helper.h"
#include "storage/vectorized/predicate_parser.h"
#include "storage/vectorized/projection_iterator.h"

namespace starrocks::vectorized {

TabletScanner::TabletScanner(OlapScanNode* parent) : _parent(parent) {}

Status TabletScanner::init(RuntimeState* runtime_state, const TabletScannerParams& params) {
    _runtime_state = runtime_state;
    _skip_aggregation = params.skip_aggregation;
    _need_agg_finalize = params.need_agg_finalize;

    RETURN_IF_ERROR(Expr::clone_if_not_exists(*params.conjunct_ctxs, runtime_state, &_conjunct_ctxs));
    RETURN_IF_ERROR(_get_tablet(params.scan_range));
    RETURN_IF_ERROR(_init_return_columns());
    RETURN_IF_ERROR(_init_global_dicts());
    RETURN_IF_ERROR(_init_reader_params(params.key_ranges));
    const TabletSchema& tablet_schema = _tablet->tablet_schema();
    Schema child_schema = ChunkHelper::convert_schema_to_format_v2(tablet_schema, _reader_columns);
    _reader = std::make_shared<TabletReader>(_tablet, Version(0, _version), std::move(child_schema));
    if (_reader_columns.size() == _scanner_columns.size()) {
        _prj_iter = _reader;
    } else {
        Schema output_schema = ChunkHelper::convert_schema_to_format_v2(tablet_schema, _scanner_columns);
        _prj_iter = new_projection_iterator(output_schema, _reader);
    }

    if (!_conjunct_ctxs.empty() || !_predicates.empty()) {
        _expr_filter_timer = ADD_TIMER(_parent->_runtime_profile, "ExprFilterTime");
    }

    DCHECK(_params.global_dictmaps != nullptr);
    RETURN_IF_ERROR(_prj_iter->init_encoded_schema(*_params.global_dictmaps));

    Status st = _reader->prepare();
    if (!st.ok()) {
        std::string msg = strings::Substitute("[$0] fail to prepare tablet reader $1: $2",
                                              BackendOptions::get_localhost(), _tablet->full_name(), st.to_string());
        LOG(WARNING) << msg;
        return Status::InternalError(msg);
    } else {
        return Status::OK();
    }
}

Status TabletScanner::open([[maybe_unused]] RuntimeState* runtime_state) {
    if (_is_open) {
        return Status::OK();
    } else {
        _is_open = true;
        Status st = _reader->open(_params);
        if (!st.ok()) {
            auto msg = strings::Substitute("[$0] fail to open tablet reader $1: $2", BackendOptions::get_localhost(),
                                           _tablet->full_name(), st.to_string());
            st = Status::InternalError(msg);
            LOG(WARNING) << st;
        }
        return st;
    }
}

Status TabletScanner::close(RuntimeState* state) {
    if (_is_closed) {
        return Status::OK();
    }
    _prj_iter->close();
    update_counter();
    _reader.reset();
    _predicate_free_pool.clear();
    Expr::close(_conjunct_ctxs, state);
    // Reduce the memory usage if the the average string size is greater than 512.
    release_large_columns<BinaryColumn>(config::vector_chunk_size * 512);
    _is_closed = true;
    return Status::OK();
}

Status TabletScanner::_get_tablet(const TInternalScanRange* scan_range) {
    TTabletId tablet_id = scan_range->tablet_id;
    SchemaHash schema_hash = strtoul(scan_range->schema_hash.c_str(), nullptr, 10);
    _version = strtoul(scan_range->version.c_str(), nullptr, 10);

    std::string err;
    _tablet = StorageEngine::instance()->tablet_manager()->get_tablet(tablet_id, schema_hash, true, &err);
    if (!_tablet) {
        std::stringstream ss;
        ss << "failed to get tablet. tablet_id=" << tablet_id << ", with schema_hash=" << schema_hash
           << ", reason=" << err;
        LOG(WARNING) << ss.str();
        return Status::InternalError(ss.str());
    }
    return Status::OK();
}

Status TabletScanner::_init_reader_params(const std::vector<OlapScanRange*>* key_ranges) {
    _params.reader_type = READER_QUERY;
    _params.skip_aggregation = _skip_aggregation;
    _params.profile = _parent->_scan_profile;
    _params.runtime_state = _runtime_state;
    // If a agg node is this scan node direct parent
    // we will not call agg object finalize method in scan node,
    // to avoid the unnecessary SerDe and improve query performance
    _params.need_agg_finalize = _need_agg_finalize;
    _params.use_page_cache = !config::disable_storage_page_cache;
    _params.chunk_size = config::vector_chunk_size;

    PredicateParser parser(_tablet->tablet_schema());
    std::vector<vectorized::ColumnPredicate*> preds;
    _parent->_conjuncts_manager.get_column_predicates(&parser, &preds);
    for (auto* p : preds) {
        _predicate_free_pool.emplace_back(p);
        if (parser.can_pushdown(p)) {
            _params.predicates.push_back(p);
        } else {
            _predicates.add(p);
        }
    }

    // Range
    for (auto key_range : *key_ranges) {
        if (key_range->begin_scan_range.size() == 1 && key_range->begin_scan_range.get_value(0) == NEGATIVE_INFINITY) {
            continue;
        }

        _params.range = key_range->begin_include ? "ge" : "gt";
        _params.end_range = key_range->end_include ? "le" : "lt";

        _params.start_key.push_back(key_range->begin_scan_range);
        _params.end_key.push_back(key_range->end_scan_range);
    }

    // Return columns
    if (_skip_aggregation) {
        _reader_columns = _scanner_columns;
    } else {
        for (size_t i = 0; i < _tablet->num_key_columns(); i++) {
            _reader_columns.push_back(i);
        }
        for (auto index : _scanner_columns) {
            if (!_tablet->tablet_schema().column(index).is_key()) {
                _reader_columns.push_back(index);
            }
        }
    }
    // Actually only the key columns need to be sorted by id, here we check all
    // for simplicity.
    DCHECK(std::is_sorted(_reader_columns.begin(), _reader_columns.end()));

    return Status::OK();
}

Status TabletScanner::_init_return_columns() {
    for (auto slot : _parent->_tuple_desc->slots()) {
        if (!slot->is_materialized()) {
            continue;
        }
        int32_t index = _tablet->field_index(slot->col_name());
        if (index < 0) {
            std::stringstream ss;
            ss << "invalid field name: " << slot->col_name();
            LOG(WARNING) << ss.str();
            return Status::InternalError(ss.str());
        }
        _scanner_columns.push_back(index);
        _query_slots.push_back(slot);
    }
    // Put key columns before non-key columns, as the `MergeIterator` and `AggregateIterator`
    // required.
    std::sort(_scanner_columns.begin(), _scanner_columns.end());
    if (_scanner_columns.empty()) {
        return Status::InternalError("failed to build storage scanner, no materialized slot!");
    }
    return Status::OK();
}

// mapping a slot-column-id to schema-columnid
Status TabletScanner::_init_global_dicts() {
    const auto& global_dict_map = _runtime_state->get_global_dict_map();
    auto global_dict = _parent->_obj_pool.add(new ColumnIdToGlobalDictMap());
    // mapping column id to storage column ids
    for (auto slot : _parent->_tuple_desc->slots()) {
        if (!slot->is_materialized()) {
            continue;
        }
        auto iter = global_dict_map.find(slot->id());
        if (iter != global_dict_map.end()) {
            auto& dict_map = iter->second.first;
            int32_t index = _tablet->field_index(slot->col_name());
            DCHECK(index >= 0);
            global_dict->emplace(index, const_cast<GlobalDictMap*>(&dict_map));
        }
    }
    _params.global_dictmaps = global_dict;

    return Status::OK();
}

Status TabletScanner::get_chunk(RuntimeState* state, Chunk* chunk) {
    if (state->is_cancelled()) {
        return Status::Cancelled("canceled state");
    }
    SCOPED_TIMER(_parent->_scan_timer);
    do {
        if (Status status = _prj_iter->get_next(chunk); !status.ok()) {
            return status;
        }
        for (auto slot : _query_slots) {
            size_t column_index = chunk->schema()->get_field_index_by_name(slot->col_name());
            chunk->set_slot_id_to_index(slot->id(), column_index);
        }

        if (!_predicates.empty()) {
            int64_t old_mem_usage = chunk->memory_usage();
            SCOPED_TIMER(_expr_filter_timer);
            size_t nrows = chunk->num_rows();
            _selection.resize(nrows);
            _predicates.evaluate(chunk, _selection.data(), 0, nrows);
            chunk->filter(_selection);
            CurrentMemTracker::consume((int64_t)chunk->memory_usage() - old_mem_usage);
            DCHECK_CHUNK(chunk);
        }
        if (!_conjunct_ctxs.empty()) {
            int64_t old_mem_usage = chunk->memory_usage();
            SCOPED_TIMER(_expr_filter_timer);
            ExecNode::eval_conjuncts(_conjunct_ctxs, chunk);
            CurrentMemTracker::consume((int64_t)chunk->memory_usage() - old_mem_usage);
            DCHECK_CHUNK(chunk);
        }
    } while (chunk->num_rows() == 0);
    _update_realtime_counter();
    return Status::OK();
}

void TabletScanner::_update_realtime_counter() {
    COUNTER_UPDATE(_parent->_read_compressed_counter, _reader->stats().compressed_bytes_read);
    _compressed_bytes_read += _reader->stats().compressed_bytes_read;
    _reader->mutable_stats()->compressed_bytes_read = 0;

    COUNTER_UPDATE(_parent->_raw_rows_counter, _reader->stats().raw_rows_read);
    _raw_rows_read += _reader->stats().raw_rows_read;
    _reader->mutable_stats()->raw_rows_read = 0;
}

void TabletScanner::update_counter() {
    if (_has_update_counter) {
        return;
    }
    COUNTER_UPDATE(_parent->_create_seg_iter_timer, _reader->stats().create_segment_iter_ns);
    COUNTER_UPDATE(_parent->_rows_read_counter, _num_rows_read);

    COUNTER_UPDATE(_parent->_io_timer, _reader->stats().io_ns);
    COUNTER_UPDATE(_parent->_read_compressed_counter, _reader->stats().compressed_bytes_read);
    _compressed_bytes_read += _reader->stats().compressed_bytes_read;
    COUNTER_UPDATE(_parent->_decompress_timer, _reader->stats().decompress_ns);
    COUNTER_UPDATE(_parent->_read_uncompressed_counter, _reader->stats().uncompressed_bytes_read);
    COUNTER_UPDATE(_parent->bytes_read_counter(), _reader->stats().bytes_read);

    COUNTER_UPDATE(_parent->_block_load_timer, _reader->stats().block_load_ns);
    COUNTER_UPDATE(_parent->_block_load_counter, _reader->stats().blocks_load);
    COUNTER_UPDATE(_parent->_block_fetch_timer, _reader->stats().block_fetch_ns);
    COUNTER_UPDATE(_parent->_block_seek_timer, _reader->stats().block_seek_ns);

    COUNTER_UPDATE(_parent->_raw_rows_counter, _reader->stats().raw_rows_read);
    _raw_rows_read += _reader->mutable_stats()->raw_rows_read;
    COUNTER_UPDATE(_parent->_chunk_copy_timer, _reader->stats().vec_cond_chunk_copy_ns);

    COUNTER_UPDATE(_parent->_seg_init_timer, _reader->stats().segment_init_ns);

    COUNTER_UPDATE(_parent->_pred_filter_timer, _reader->stats().vec_cond_evaluate_ns);
    COUNTER_UPDATE(_parent->_pred_filter_counter, _reader->stats().rows_vec_cond_filtered);
    COUNTER_UPDATE(_parent->_del_vec_filter_counter, _reader->stats().rows_del_vec_filtered);
    COUNTER_UPDATE(_parent->_seg_zm_filtered_counter, _reader->stats().segment_stats_filtered);
    COUNTER_UPDATE(_parent->_zm_filtered_counter, _reader->stats().rows_stats_filtered);
    COUNTER_UPDATE(_parent->_bf_filtered_counter, _reader->stats().rows_bf_filtered);
    COUNTER_UPDATE(_parent->_sk_filtered_counter, _reader->stats().rows_key_range_filtered);
    COUNTER_UPDATE(_parent->_index_load_timer, _reader->stats().index_load_ns);

    COUNTER_UPDATE(_parent->_total_pages_num_counter, _reader->stats().total_pages_num);
    COUNTER_UPDATE(_parent->_cached_pages_num_counter, _reader->stats().cached_pages_num);

    COUNTER_UPDATE(_parent->_bi_filtered_counter, _reader->stats().rows_bitmap_index_filtered);
    COUNTER_UPDATE(_parent->_bi_filter_timer, _reader->stats().bitmap_index_filter_timer);
    COUNTER_UPDATE(_parent->_block_seek_counter, _reader->stats().block_seek_num);

    COUNTER_SET(_parent->_pushdown_predicates_counter, (int64_t)_params.predicates.size());

    StarRocksMetrics::instance()->query_scan_bytes.increment(_compressed_bytes_read);
    StarRocksMetrics::instance()->query_scan_rows.increment(_raw_rows_read);

    if (_reader->stats().decode_dict_ns > 0) {
        RuntimeProfile::Counter* c = ADD_TIMER(_parent->_scan_profile, "DictDecode");
        COUNTER_UPDATE(c, _reader->stats().decode_dict_ns);
    }
    if (_reader->stats().late_materialize_ns > 0) {
        RuntimeProfile::Counter* c = ADD_TIMER(_parent->_scan_profile, "LateMaterialize");
        COUNTER_UPDATE(c, _reader->stats().late_materialize_ns);
    }
    if (_reader->stats().del_filter_ns > 0) {
        RuntimeProfile::Counter* c1 = ADD_TIMER(_parent->_scan_profile, "DeleteFilter");
        RuntimeProfile::Counter* c2 = ADD_COUNTER(_parent->_scan_profile, "DeleteFilterRows", TUnit::UNIT);
        COUNTER_UPDATE(c1, _reader->stats().del_filter_ns);
        COUNTER_UPDATE(c2, _reader->stats().rows_del_filtered);
    }
    _has_update_counter = true;
}

} // namespace starrocks::vectorized

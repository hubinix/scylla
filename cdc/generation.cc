/*
 * Copyright (C) 2019 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <boost/type.hpp>
#include <random>
#include <unordered_set>
#include <algorithm>
#include <seastar/core/sleep.hh>
#include <seastar/core/coroutine.hh>

#include "keys.hh"
#include "schema_builder.hh"
#include "database.hh"
#include "db/config.hh"
#include "db/system_keyspace.hh"
#include "db/system_distributed_keyspace.hh"
#include "dht/token-sharding.hh"
#include "locator/token_metadata.hh"
#include "gms/application_state.hh"
#include "gms/inet_address.hh"
#include "gms/gossiper.hh"

#include "cdc/generation.hh"
#include "cdc/cdc_options.hh"

extern logging::logger cdc_log;

static int get_shard_count(const gms::inet_address& endpoint, const gms::gossiper& g) {
    auto ep_state = g.get_application_state_ptr(endpoint, gms::application_state::SHARD_COUNT);
    return ep_state ? std::stoi(ep_state->value) : -1;
}

static unsigned get_sharding_ignore_msb(const gms::inet_address& endpoint, const gms::gossiper& g) {
    auto ep_state = g.get_application_state_ptr(endpoint, gms::application_state::IGNORE_MSB_BITS);
    return ep_state ? std::stoi(ep_state->value) : 0;
}

namespace cdc {

extern const api::timestamp_clock::duration generation_leeway =
    std::chrono::duration_cast<api::timestamp_clock::duration>(std::chrono::seconds(5));

static void copy_int_to_bytes(int64_t i, size_t offset, bytes& b) {
    i = net::hton(i);
    std::copy_n(reinterpret_cast<int8_t*>(&i), sizeof(int64_t), b.begin() + offset);
}

static constexpr auto stream_id_version_bits = 4;
static constexpr auto stream_id_random_bits = 38;
static constexpr auto stream_id_index_bits = sizeof(uint64_t)*8 - stream_id_version_bits - stream_id_random_bits;

static constexpr auto stream_id_version_shift = 0;
static constexpr auto stream_id_index_shift = stream_id_version_shift + stream_id_version_bits;
static constexpr auto stream_id_random_shift = stream_id_index_shift + stream_id_index_bits;

/**
 * Responsibilty for encoding stream_id moved from factory method to
 * this constructor, to keep knowledge of composition in a single place.
 * Note this is private and friended to topology_description_generator,
 * because he is the one who defined the "order" we view vnodes etc.
 */
stream_id::stream_id(dht::token token, size_t vnode_index)
    : _value(bytes::initialized_later(), 2 * sizeof(int64_t))
{
    static thread_local std::mt19937_64 rand_gen(std::random_device{}());
    static thread_local std::uniform_int_distribution<uint64_t> rand_dist;

    auto rand = rand_dist(rand_gen);
    auto mask_shift = [](uint64_t val, size_t bits, size_t shift) {
        return (val & ((1ull << bits) - 1u)) << shift;
    };
    /**
     *  Low qword:
     * 0-4: version
     * 5-26: vnode index as when created (see generation below). This excludes shards
     * 27-64: random value (maybe to be replaced with timestamp)
     */
    auto low_qword = mask_shift(version_1, stream_id_version_bits, stream_id_version_shift)
        | mask_shift(vnode_index, stream_id_index_bits, stream_id_index_shift)
        | mask_shift(rand, stream_id_random_bits, stream_id_random_shift)
        ;

    copy_int_to_bytes(dht::token::to_int64(token), 0, _value);
    copy_int_to_bytes(low_qword, sizeof(int64_t), _value);
    // not a hot code path. make sure we did not mess up the shifts and masks.
    assert(version() == version_1);
    assert(index() == vnode_index);
}

stream_id::stream_id(bytes b)
    : _value(std::move(b))
{
    // this is not a very solid check. Id:s previous to GA/versioned id:s
    // have fully random bits in low qword, so this could go either way...
    if (version() > version_1) {
        throw std::invalid_argument("Unknown CDC stream id version");
    }
}

bool stream_id::is_set() const {
    return !_value.empty();
}

bool stream_id::operator==(const stream_id& o) const {
    return _value == o._value;
}

bool stream_id::operator!=(const stream_id& o) const {
    return !(*this == o);
}

bool stream_id::operator<(const stream_id& o) const {
    return _value < o._value;
}

static int64_t bytes_to_int64(bytes_view b, size_t offset) {
    assert(b.size() >= offset + sizeof(int64_t));
    int64_t res;
    std::copy_n(b.begin() + offset, sizeof(int64_t), reinterpret_cast<int8_t *>(&res));
    return net::ntoh(res);
}

dht::token stream_id::token() const {
    return dht::token::from_int64(token_from_bytes(_value));
}

int64_t stream_id::token_from_bytes(bytes_view b) {
    return bytes_to_int64(b, 0);
}

static uint64_t unpack_value(bytes_view b, size_t off, size_t shift, size_t bits) {
    return (uint64_t(bytes_to_int64(b, off)) >> shift) & ((1ull << bits) - 1u);
}

uint8_t stream_id::version() const {
    return unpack_value(_value, sizeof(int64_t), stream_id_version_shift, stream_id_version_bits);
}

size_t stream_id::index() const {
    return unpack_value(_value, sizeof(int64_t), stream_id_index_shift, stream_id_index_bits);
}

const bytes& stream_id::to_bytes() const {
    return _value;
}

partition_key stream_id::to_partition_key(const schema& log_schema) const {
    return partition_key::from_single_value(log_schema, _value);
}

bool token_range_description::operator==(const token_range_description& o) const {
    return token_range_end == o.token_range_end && streams == o.streams
        && sharding_ignore_msb == o.sharding_ignore_msb;
}

topology_description::topology_description(std::vector<token_range_description> entries)
    : _entries(std::move(entries)) {}

bool topology_description::operator==(const topology_description& o) const {
    return _entries == o._entries;
}

const std::vector<token_range_description>& topology_description::entries() const& {
    return _entries;
}

std::vector<token_range_description>&& topology_description::entries() && {
    return std::move(_entries);
}

static std::vector<stream_id> create_stream_ids(
        size_t index, dht::token start, dht::token end, size_t shard_count, uint8_t ignore_msb) {
    std::vector<stream_id> result;
    result.reserve(shard_count);
    dht::sharder sharder(shard_count, ignore_msb);
    for (size_t shard_idx = 0; shard_idx < shard_count; ++shard_idx) {
        auto t = dht::find_first_token_for_shard(sharder, start, end, shard_idx);
        // compose the id from token and the "index" of the range end owning vnode
        // as defined by token sort order. Basically grouping within this
        // shard set.
        result.emplace_back(stream_id(t, index));
    }
    return result;
}

class topology_description_generator final {
    const db::config& _cfg;
    const std::unordered_set<dht::token>& _bootstrap_tokens;
    const locator::token_metadata_ptr _tmptr;
    const gms::gossiper& _gossiper;

    // Compute a set of tokens that split the token ring into vnodes
    auto get_tokens() const {
        auto tokens = _tmptr->sorted_tokens();
        auto it = tokens.insert(
                tokens.end(), _bootstrap_tokens.begin(), _bootstrap_tokens.end());
        std::sort(it, tokens.end());
        std::inplace_merge(tokens.begin(), it, tokens.end());
        tokens.erase(std::unique(tokens.begin(), tokens.end()), tokens.end());
        return tokens;
    }

    // Fetch sharding parameters for a node that owns vnode ending with this.end
    // Returns <shard_count, ignore_msb> pair.
    std::pair<size_t, uint8_t> get_sharding_info(dht::token end) const {
        if (_bootstrap_tokens.contains(end)) {
            return {smp::count, _cfg.murmur3_partitioner_ignore_msb_bits()};
        } else {
            auto endpoint = _tmptr->get_endpoint(end);
            if (!endpoint) {
                throw std::runtime_error(
                        format("Can't find endpoint for token {}", end));
            }
            auto sc = get_shard_count(*endpoint, _gossiper);
            return {sc > 0 ? sc : 1, get_sharding_ignore_msb(*endpoint, _gossiper)};
        }
    }

    token_range_description create_description(size_t index, dht::token start, dht::token end) const {
        token_range_description desc;

        desc.token_range_end = end;

        auto [shard_count, ignore_msb] = get_sharding_info(end);
        desc.streams = create_stream_ids(index, start, end, shard_count, ignore_msb);
        desc.sharding_ignore_msb = ignore_msb;

        return desc;
    }
public:
    topology_description_generator(
            const db::config& cfg,
            const std::unordered_set<dht::token>& bootstrap_tokens,
            const locator::token_metadata_ptr tmptr,
            const gms::gossiper& gossiper)
        : _cfg(cfg)
        , _bootstrap_tokens(bootstrap_tokens)
        , _tmptr(std::move(tmptr))
        , _gossiper(gossiper)
    {}

    /*
     * Generate a set of CDC stream identifiers such that for each shard
     * and vnode pair there exists a stream whose token falls into this vnode
     * and is owned by this shard. It is sometimes not possible to generate
     * a CDC stream identifier for some (vnode, shard) pair because not all
     * shards have to own tokens in a vnode. Small vnode can be totally owned
     * by a single shard. In such case, a stream identifier that maps to
     * end of the vnode is generated.
     *
     * Then build a cdc::topology_description which maps tokens to generated
     * stream identifiers, such that if token T is owned by shard S in vnode V,
     * it gets mapped to the stream identifier generated for (S, V).
     */
    // Run in seastar::async context.
    topology_description generate() const {
        const auto tokens = get_tokens();

        std::vector<token_range_description> vnode_descriptions;
        vnode_descriptions.reserve(tokens.size());

        vnode_descriptions.push_back(
                create_description(0, tokens.back(), tokens.front()));
        for (size_t idx = 1; idx < tokens.size(); ++idx) {
            vnode_descriptions.push_back(
                    create_description(idx, tokens[idx - 1], tokens[idx]));
        }

        return {std::move(vnode_descriptions)};
    }
};

bool should_propose_first_generation(const gms::inet_address& me, const gms::gossiper& g) {
    auto my_host_id = g.get_host_id(me);
    auto& eps = g.get_endpoint_states();
    return std::none_of(eps.begin(), eps.end(),
            [&] (const std::pair<gms::inet_address, gms::endpoint_state>& ep) {
        return my_host_id < g.get_host_id(ep.first);
    });
}

future<db_clock::time_point> get_local_streams_timestamp() {
    return db::system_keyspace::get_saved_cdc_streams_timestamp().then([] (std::optional<db_clock::time_point> ts) {
        if (!ts) {
            auto err = format("get_local_streams_timestamp: tried to retrieve streams timestamp after bootstrapping, but it's not present");
            cdc_log.error("{}", err);
            throw std::runtime_error(err);
        }
        return *ts;
    });
}

// non-static for testing
size_t limit_of_streams_in_topology_description() {
    // Each stream takes 16B and we don't want to exceed 4MB so we can have
    // at most 262144 streams but not less than 1 per vnode.
    return 4 * 1024 * 1024 / 16;
}

// non-static for testing
topology_description limit_number_of_streams_if_needed(topology_description&& desc) {
    int64_t streams_count = 0;
    for (auto& tr_desc : desc.entries()) {
        streams_count += tr_desc.streams.size();
    }

    size_t limit = std::max(limit_of_streams_in_topology_description(), desc.entries().size());
    if (limit >= streams_count) {
        return std::move(desc);
    }
    size_t streams_per_vnode_limit = limit / desc.entries().size();
    auto entries = std::move(desc).entries();
    auto start = entries.back().token_range_end;
    for (size_t idx = 0; idx < entries.size(); ++idx) {
        auto end = entries[idx].token_range_end;
        if (entries[idx].streams.size() > streams_per_vnode_limit) {
            entries[idx].streams =
                create_stream_ids(idx, start, end, streams_per_vnode_limit, entries[idx].sharding_ignore_msb);
        }
        start = end;
    }
    return topology_description(std::move(entries));
}

// Run inside seastar::async context.
db_clock::time_point make_new_cdc_generation(
        const db::config& cfg,
        const std::unordered_set<dht::token>& bootstrap_tokens,
        const locator::token_metadata_ptr tmptr,
        const gms::gossiper& g,
        db::system_distributed_keyspace& sys_dist_ks,
        std::chrono::milliseconds ring_delay,
        bool add_delay) {
    using namespace std::chrono;
    auto gen = topology_description_generator(cfg, bootstrap_tokens, tmptr, g).generate();

    // If the cluster is large we may end up with a generation that contains
    // large number of streams. This is problematic because we store the
    // generation in a single row. For a generation with large number of rows
    // this will lead to a row that can be as big as 32MB. This is much more
    // than the limit imposed by commitlog_segment_size_in_mb. If the size of
    // the row that describes a new generation grows above
    // commitlog_segment_size_in_mb, the write will fail and the new node won't
    // be able to join. To avoid such problem we make sure that such row is
    // always smaller than 4MB. We do that by removing some CDC streams from
    // each vnode if the total number of streams is too large.
    gen = limit_number_of_streams_if_needed(std::move(gen));

    // Begin the race.
    auto ts = db_clock::now() + (
            (!add_delay || ring_delay == milliseconds(0)) ? milliseconds(0) : (
                2 * ring_delay + duration_cast<milliseconds>(generation_leeway)));
    sys_dist_ks.insert_cdc_topology_description(ts, std::move(gen), { tmptr->count_normal_token_owners() }).get();

    return ts;
}

std::optional<db_clock::time_point> get_streams_timestamp_for(const gms::inet_address& endpoint, const gms::gossiper& g) {
    auto streams_ts_string = g.get_application_state_value(endpoint, gms::application_state::CDC_STREAMS_TIMESTAMP);
    cdc_log.trace("endpoint={}, streams_ts_string={}", endpoint, streams_ts_string);
    return gms::versioned_value::cdc_streams_timestamp_from_string(streams_ts_string);
}

static future<> do_update_streams_description(
        db_clock::time_point streams_ts,
        db::system_distributed_keyspace& sys_dist_ks,
        db::system_distributed_keyspace::context ctx) {
    if (co_await sys_dist_ks.cdc_desc_exists(streams_ts, ctx)) {
        cdc_log.info("Generation {}: streams description table already updated.", streams_ts);
        co_return;
    }

    // We might race with another node also inserting the description, but that's ok. It's an idempotent operation.

    auto topo = co_await sys_dist_ks.read_cdc_topology_description(streams_ts, ctx);
    if (!topo) {
        throw no_generation_data_exception(streams_ts);
    }

    co_await sys_dist_ks.create_cdc_desc(streams_ts, *topo, ctx);
    cdc_log.info("CDC description table successfully updated with generation {}.", streams_ts);
}

void update_streams_description(
        db_clock::time_point streams_ts,
        shared_ptr<db::system_distributed_keyspace> sys_dist_ks,
        noncopyable_function<unsigned()> get_num_token_owners,
        abort_source& abort_src) {
    try {
        do_update_streams_description(streams_ts, *sys_dist_ks, { get_num_token_owners() }).get();
    } catch(...) {
        cdc_log.warn(
            "Could not update CDC description table with generation {}: {}. Will retry in the background.",
            streams_ts, std::current_exception());

        // It is safe to discard this future: we keep system distributed keyspace alive.
        (void)seastar::async([
            streams_ts, sys_dist_ks, get_num_token_owners = std::move(get_num_token_owners), &abort_src
        ] {
            while (true) {
                sleep_abortable(std::chrono::seconds(60), abort_src).get();
                try {
                    do_update_streams_description(streams_ts, *sys_dist_ks, { get_num_token_owners() }).get();
                    return;
                } catch (...) {
                    cdc_log.warn(
                        "Could not update CDC description table with generation {}: {}. Will try again.",
                        streams_ts, std::current_exception());
                }
            }
        });
    }
}

static db_clock::time_point as_timepoint(const utils::UUID& uuid) {
    return db_clock::time_point{std::chrono::milliseconds(utils::UUID_gen::get_adjusted_timestamp(uuid))};
}

static future<std::vector<db_clock::time_point>> get_cdc_desc_v1_timestamps(
        db::system_distributed_keyspace& sys_dist_ks,
        abort_source& abort_src,
        const noncopyable_function<unsigned()>& get_num_token_owners) {
    while (true) {
        try {
            co_return co_await sys_dist_ks.get_cdc_desc_v1_timestamps({ get_num_token_owners() });
        } catch (...) {
            cdc_log.warn(
                    "Failed to retrieve generation timestamps for rewriting: {}. Retrying in 60s.",
                    std::current_exception());
        }
        co_await sleep_abortable(std::chrono::seconds(60), abort_src);
    }
}

// Contains a CDC log table's creation time (extracted from its schema's id)
// and its CDC TTL setting.
struct time_and_ttl {
    db_clock::time_point creation_time;
    int ttl;
};

/*
 * See `maybe_rewrite_streams_descriptions`.
 * This is the long-running-in-the-background part of that function.
 * It returns the timestamp of the last rewritten generation (if any).
 */
static future<std::optional<db_clock::time_point>> rewrite_streams_descriptions(
        std::vector<time_and_ttl> times_and_ttls,
        shared_ptr<db::system_distributed_keyspace> sys_dist_ks,
        noncopyable_function<unsigned()> get_num_token_owners,
        abort_source& abort_src) {
    cdc_log.info("Retrieving generation timestamps for rewriting...");
    auto tss = co_await get_cdc_desc_v1_timestamps(*sys_dist_ks, abort_src, get_num_token_owners);
    cdc_log.info("Generation timestamps retrieved.");

    // Find first generation timestamp such that some CDC log table may contain data before this timestamp.
    // This predicate is monotonic w.r.t the timestamps.
    auto now = db_clock::now();
    std::sort(tss.begin(), tss.end());
    auto first = std::partition_point(tss.begin(), tss.end(), [&] (db_clock::time_point ts) {
        // partition_point finds first element that does *not* satisfy the predicate.
        return std::none_of(times_and_ttls.begin(), times_and_ttls.end(),
                [&] (const time_and_ttl& tat) {
            // In this CDC log table there are no entries older than the table's creation time
            // or (now - the table's ttl). We subtract 10s to account for some possible clock drift.
            // If ttl is set to 0 then entries in this table never expire. In that case we look
            // only at the table's creation time.
            auto no_entries_older_than =
                (tat.ttl == 0 ? tat.creation_time : std::max(tat.creation_time, now - std::chrono::seconds(tat.ttl)))
                    - std::chrono::seconds(10);
            return no_entries_older_than < ts;
        });
    });

    // Find first generation timestamp such that some CDC log table may contain data in this generation.
    // This and all later generations need to be written to the new streams table.
    if (first != tss.begin()) {
        --first;
    }

    if (first == tss.end()) {
        cdc_log.info("No generations to rewrite.");
        co_return std::nullopt;
    }

    cdc_log.info("First generation to rewrite: {}", *first);

    bool each_success = true;
    co_await max_concurrent_for_each(first, tss.end(), 10, [&] (db_clock::time_point ts) -> future<> {
        while (true) {
            try {
                co_return co_await do_update_streams_description(ts, *sys_dist_ks, { get_num_token_owners() });
            } catch (const no_generation_data_exception& e) {
                cdc_log.error("Failed to rewrite streams for generation {}: {}. Giving up.", ts, e);
                each_success = false;
                co_return;
            } catch (...) {
                cdc_log.warn("Failed to rewrite streams for generation {}: {}. Retrying in 60s.", ts, std::current_exception());
            }
            co_await sleep_abortable(std::chrono::seconds(60), abort_src);
        }
    });

    if (each_success) {
        cdc_log.info("Rewriting stream tables finished successfully.");
    } else {
        cdc_log.info("Rewriting stream tables finished, but some generations could not be rewritten (check the logs).");
    }

    if (first != tss.end()) {
        co_return *std::prev(tss.end());
    }

    co_return std::nullopt;
}

future<> maybe_rewrite_streams_descriptions(
        const database& db,
        shared_ptr<db::system_distributed_keyspace> sys_dist_ks,
        noncopyable_function<unsigned()> get_num_token_owners,
        abort_source& abort_src) {
    if (!db.has_schema(sys_dist_ks->NAME, sys_dist_ks->CDC_DESC_V1)) {
        // This cluster never went through a Scylla version which used this table
        // or the user deleted the table. Nothing to do.
        co_return;
    }

    if (co_await db::system_keyspace::cdc_is_rewritten()) {
        co_return;
    }

    if (db.get_config().cdc_dont_rewrite_streams()) {
        cdc_log.warn("Stream rewriting disabled. Manual administrator intervention may be required...");
        co_return;
    }

    // For each CDC log table get the TTL setting (from CDC options) and the table's creation time
    std::vector<time_and_ttl> times_and_ttls;
    for (auto& [_, cf] : db.get_column_families()) {
        auto& s = *cf->schema();
        auto base = cdc::get_base_table(db, s.ks_name(), s.cf_name());
        if (!base) {
            // Not a CDC log table.
            continue;
        }
        auto& cdc_opts = base->cdc_options();
        if (!cdc_opts.enabled()) {
            // This table is named like a CDC log table but it's not one.
            continue;
        }

        times_and_ttls.push_back(time_and_ttl{as_timepoint(s.id()), cdc_opts.ttl()});
    }

    if (times_and_ttls.empty()) {
        // There's no point in rewriting old generations' streams (they don't contain any data).
        cdc_log.info("No CDC log tables present, not rewriting stream tables.");
        co_return co_await db::system_keyspace::cdc_set_rewritten(std::nullopt);
    }

    // It's safe to discard this future: the coroutine keeps system_distributed_keyspace alive
    // and the abort source's lifetime extends the lifetime of any other service.
    (void)(([_times_and_ttls = std::move(times_and_ttls), _sys_dist_ks = std::move(sys_dist_ks),
                _get_num_token_owners = std::move(get_num_token_owners), &_abort_src = abort_src] () mutable -> future<> {
        auto times_and_ttls = std::move(_times_and_ttls);
        auto sys_dist_ks = std::move(_sys_dist_ks);
        auto get_num_token_owners = std::move(_get_num_token_owners);
        auto& abort_src = _abort_src;

        // This code is racing with node startup. At this point, we're most likely still waiting for gossip to settle
        // and some nodes that are UP may still be marked as DOWN by us.
        // Let's sleep a bit to increase the chance that the first attempt at rewriting succeeds (it's still ok if
        // it doesn't - we'll retry - but it's nice if we succeed without any warnings).
        co_await sleep_abortable(std::chrono::seconds(10), abort_src);

        cdc_log.info("Rewriting stream tables in the background...");
        auto last_rewritten = co_await rewrite_streams_descriptions(
                std::move(times_and_ttls),
                std::move(sys_dist_ks),
                std::move(get_num_token_owners),
                abort_src);

        co_await db::system_keyspace::cdc_set_rewritten(last_rewritten);
    })());
}

} // namespace cdc

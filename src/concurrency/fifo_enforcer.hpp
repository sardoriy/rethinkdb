#ifndef CONCURRENCY_FIFO_ENFORCER_HPP_
#define CONCURRENCY_FIFO_ENFORCER_HPP_

#include <map>

#include "concurrency/mutex_assertion.hpp"
#include "rpc/serialize_macros.hpp"
#include "concurrency/signal.hpp"
#include "timestamps.hpp"
#include "utils.hpp"
#include "concurrency/queue/passive_producer.hpp"
#include "stl_utils.hpp"
#include "perfmon.hpp"

class cond_t;
class signal_t;

namespace unittest {
void run_queue_equivalence_test();
}

/* `fifo_enforcer.hpp` contains facilities for enforcing that objects pass
through a checkpoint in the same order that they passed through a previous
checkpoint. The objects in transit between the checkpoints are identified by
"tokens", of the types `fifo_enforcer_read_token_t` and
`fifo_enforcer_write_token_t`. Read tokens are allowed to be reordered relative
to each other but not relative to write tokens. */

/* A clinic can be used as metaphor for how `fifo_enforcer_{source,sink}_t` are used.
 * First you get an appointment (`fifo_enforcer_source_t::enter_{read,write}()`), then you
 * come to a waiting room (construct `fifo_enforcer_sink_t::exit_{read,write}_t`), where
 * you are notified when the doctor is ready to see you (the `exit_{read,write}_t` object
 * gets pulsed). When you finish seeing the doctor (destroy the `exit_{read,write}_t`),
 * the person who has the next appointment is notified.  If you leave the waiting room
 * (destroy the `exit_{read,write}_t` before it is pulsed), you forfeit your appointment
 * and place in line.
 *
 * The metaphor breaks down in that, if you don't show up for your appointment, all the
 * later appointments are delayed indefinitely. That means that if you never construct a
 * `exit_{read,write}_t` for a token, users of all later tokens will never get to go.
 */
class fifo_enforcer_read_token_t {
public:
    fifo_enforcer_read_token_t() THROWS_NOTHING { }
private:
    friend class fifo_enforcer_source_t;
    friend class fifo_enforcer_sink_t;
    template <class T>
    friend class fifo_enforcer_queue_t;
    explicit fifo_enforcer_read_token_t(state_timestamp_t t) THROWS_NOTHING :
        timestamp(t) { }
    state_timestamp_t timestamp;
    RDB_MAKE_ME_SERIALIZABLE_1(timestamp);
};

class fifo_enforcer_write_token_t {
public:
    fifo_enforcer_write_token_t() THROWS_NOTHING : timestamp(), num_preceding_reads(-1) { }
private:
    friend class fifo_enforcer_source_t;
    friend class fifo_enforcer_sink_t;
    template <class T>
    friend class fifo_enforcer_queue_t;
    fifo_enforcer_write_token_t(transition_timestamp_t t, int npr) THROWS_NOTHING :
        timestamp(t), num_preceding_reads(npr) { }
    transition_timestamp_t timestamp;
    int64_t num_preceding_reads;
    RDB_MAKE_ME_SERIALIZABLE_2(timestamp, num_preceding_reads);
};

class fifo_enforcer_source_t : public home_thread_mixin_t {
public:
    /* `state_t` represents the internal state of a `fifo_enforcer_source_t`. If
    you want to initialize a `fifo_enforcer_sink_t` in such a way that it
    "skips" all of the tokens that have already been generated by the
    `fifo_enforcer_source_t`, then call `get_state()` on the
    `fifo_enforcer_source_t` and pass the result to the `fifo_enforcer_sink_t`
    constructor. */
    class state_t {
    public:
        state_t() THROWS_NOTHING { }
    private:
        friend class fifo_enforcer_source_t;
        friend class fifo_enforcer_sink_t;
        template <class T>
        friend class fifo_enforcer_queue_t;
        state_t(state_timestamp_t ts, int64_t nr) THROWS_NOTHING : timestamp(ts), num_reads(nr) { }
        state_timestamp_t timestamp;
        int64_t num_reads;
        RDB_MAKE_ME_SERIALIZABLE_2(timestamp, num_reads);
    };

    fifo_enforcer_source_t() THROWS_NOTHING :
        state(state_timestamp_t::zero(), 0) { }

    /* Enters the FIFO for read. Does not block. */
    fifo_enforcer_read_token_t enter_read() THROWS_NOTHING;

    /* Enters the FIFO for write. Does not block. */
    fifo_enforcer_write_token_t enter_write() THROWS_NOTHING;

    state_t get_state() THROWS_NOTHING {
        return state;
    }

private:
    mutex_assertion_t lock;
    state_t state;
    DISABLE_COPYING(fifo_enforcer_source_t);
};

class fifo_enforcer_sink_t : public home_thread_mixin_t {
public:
    class exit_read_t;
    class exit_write_t;
private:
    typedef std::multimap<state_timestamp_t, exit_read_t*> reader_queue_t;
    typedef std::map<transition_timestamp_t, std::pair<int64_t, exit_write_t*> > writer_queue_t;
public:
    class exit_read_t : public signal_t {
    public:
        exit_read_t(fifo_enforcer_sink_t *, fifo_enforcer_read_token_t) THROWS_NOTHING;
        ~exit_read_t() THROWS_NOTHING;

        void reset();
    private:
        friend class fifo_enforcer_sink_t;

        // parent is null, once the exit_read_t has been reset().
        fifo_enforcer_sink_t *parent;

        fifo_enforcer_read_token_t token;
        reader_queue_t::iterator queue_position;
    };

    class exit_write_t : public signal_t {
    public:
        exit_write_t(fifo_enforcer_sink_t *, fifo_enforcer_write_token_t) THROWS_NOTHING;
        ~exit_write_t() THROWS_NOTHING;

        void reset();
    private:
        friend class fifo_enforcer_sink_t;

        // parent is null, once the exit_write_t has been reset().
        fifo_enforcer_sink_t *parent;

        fifo_enforcer_write_token_t token;
        writer_queue_t::iterator queue_position;
    };

    fifo_enforcer_sink_t() THROWS_NOTHING :
        state(state_timestamp_t::zero(), 0) { }

    explicit fifo_enforcer_sink_t(fifo_enforcer_source_t::state_t init) THROWS_NOTHING :
        state(init) { }

    ~fifo_enforcer_sink_t() THROWS_NOTHING {
        for (reader_queue_t::iterator it =  waiting_readers.begin();
                                      it != waiting_readers.end();
                                      it++) {
            /* Make sure that any entries present are dummy entries, ie null
             * pointer. If there are non dummy entries that means there are
             * living exit_read_t objects that point to us. */
            rassert(!it->second);
        }

        for (writer_queue_t::iterator it =  waiting_writers.begin();
                                      it != waiting_writers.end();
                                      it++) {
            /* Make sure that any entries present are dummy entries, ie null
             * pointer. If there are non dummy entries that means there are
             * living exit_write_t objects that point to us. */
            rassert(!it->second.second);
        }
    }

private:
    void pump() THROWS_NOTHING;
    void finish_a_reader(state_timestamp_t timestamp) THROWS_NOTHING;
    void finish_a_writer(transition_timestamp_t timestamp, int64_t num_preceding_reads) THROWS_NOTHING;

    mutex_assertion_t lock;
    fifo_enforcer_source_t::state_t state;
    reader_queue_t waiting_readers;
    writer_queue_t waiting_writers;
    DISABLE_COPYING(fifo_enforcer_sink_t);
};

//TODO no reason for this to be a template. Make it in to a boost::function
template <class T>
class fifo_enforcer_queue_t : public passive_producer_t<T>, public home_thread_mixin_t {
public:
    explicit fifo_enforcer_queue_t();
    fifo_enforcer_queue_t(perfmon_counter_t *_read_counter, perfmon_counter_t *_write_counter);
    ~fifo_enforcer_queue_t();

    void push(fifo_enforcer_read_token_t token, const T &t); 
    void finish_read(fifo_enforcer_read_token_t read_token); 
    void push(fifo_enforcer_write_token_t token, const T &t); 
    void finish_write(fifo_enforcer_write_token_t write_token); 

private:
    typedef std::multimap<state_timestamp_t, T> read_queue_t;
    read_queue_t read_queue;

    typedef std::map<transition_timestamp_t, std::pair<int64_t, T> > write_queue_t;
    write_queue_t write_queue;

    mutex_assertion_t lock;

    fifo_enforcer_source_t::state_t state;

    perfmon_counter_t *read_counter, *write_counter;

private:
friend void unittest::run_queue_equivalence_test();
    //passive produce api
    T produce_next_value(); 

    availability_control_t control;

    void consider_changing_available(); 

    DISABLE_COPYING(fifo_enforcer_queue_t);
};

#include "concurrency/fifo_enforcer.tcc"

#endif /* CONCURRENCY_FIFO_ENFORCER_HPP_ */

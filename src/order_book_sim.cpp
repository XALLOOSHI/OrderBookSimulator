// order_book_sim.cpp
// Simple high-performance order book simulator with a threaded load generator.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using ns = std::chrono::nanoseconds;
using steady_clock = std::chrono::steady_clock;

enum class Side
{
    BUY,
    SELL
};
enum class Type
{
    LIMIT,
    MARKET,
    IOC
};

struct Order
{
    uint64_t id;
    Side side;
    Type type;
    double price;      // for limit orders; ignored for market orders
    uint64_t qty;      // remaining quantity
    uint64_t orig_qty; // original quantity
    uint64_t ts;       // logical timestamp for time priority

    Order() = default;
    Order(uint64_t id_, Side s, Type t, double p, uint64_t q, uint64_t ts_)
        : id(id_), side(s), type(t), price(p), qty(q), orig_qty(q), ts(ts_) {}
};

struct Trade
{
    uint64_t buy_id;
    uint64_t sell_id;
    double price;
    uint64_t qty;
    uint64_t ts;
};

static inline string side_to_str(Side s) { return s == Side::BUY ? "BUY" : "SELL"; }
static inline string type_to_str(Type t)
{
    if (t == Type::LIMIT)
        return "LIMIT";
    if (t == Type::MARKET)
        return "MARKET";
    return "IOC";
}

// Thread-safe queue
template <typename T>
class TSQueue
{
public:
    void push(T item)
    {
        unique_lock<mutex> lock(mtx);
        q.push_back(std::move(item));
        cv.notify_one();
    }

    bool pop(T &out)
    {
        unique_lock<mutex> lock(mtx);
        cv.wait(lock, [&]
                { return !q.empty() || finished; });
        if (q.empty())
            return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }

    void set_finished()
    {
        unique_lock<mutex> lock(mtx);
        finished = true;
        cv.notify_all();
    }

private:
    deque<T> q;
    mutex mtx;
    condition_variable cv;
    bool finished = false;
};

// OrderBook: maps price -> deque<Order>
// bids: descending price (best = highest)
// asks: ascending price (best = lowest)
class OrderBook
{
public:
    OrderBook() : next_ts(1) {}

    vector<Trade> process_order(Order order)
    {
        order.ts = next_ts++;
        vector<Trade> trades;
        if (order.side == Side::BUY)
        {
            match_order(order, asks, bids, Side::SELL, trades);
        }
        else
        {
            match_order(order, bids, asks, Side::BUY, trades);
        }
        if (order.qty > 0 && order.type == Type::LIMIT)
        {
            insert_limit(order);
        }
        return trades;
    }

    void print_top(size_t depth = 5)
    {
        cout << "=== Order Book Top " << depth << " ===\n";
        cout << "ASKS (price asc):\n";
        size_t c = 0;
        for (auto it = asks.begin(); it != asks.end() && c < depth; ++it, ++c)
        {
            uint64_t total = 0;
            for (auto &o : it->second)
                total += o.qty;
            cout << "  " << fixed << setprecision(2) << it->first << " x " << total << "\n";
        }
        cout << "BIDS (price desc):\n";
        c = 0;
        for (auto it = bids.begin(); it != bids.end() && c < depth; ++it, ++c)
        {
            uint64_t total = 0;
            for (auto &o : it->second)
                total += o.qty;
            cout << "  " << fixed << setprecision(2) << it->first << " x " << total << "\n";
        }
    }

private:
    map<double, deque<Order>> asks;
    map<double, deque<Order>, greater<double>> bids;
    atomic<uint64_t> next_ts;

    void insert_limit(const Order &order)
    {
        if (order.side == Side::BUY)
        {
            bids[order.price].push_back(order);
        }
        else
        {
            asks[order.price].push_back(order);
        }
    }

    void match_order(Order &incoming,
                     map<double, deque<Order>> &opposite_book,
                     map<double, deque<Order>, greater<double>> &own_book,
                     Side opposite_side,
                     vector<Trade> &trades)
    {
        auto it = opposite_book.begin();
        while (incoming.qty > 0 && it != opposite_book.end())
        {
            double level_price = it->first;
            bool price_ok = false;
            if (incoming.type == Type::MARKET)
                price_ok = true;
            else if (incoming.side == Side::BUY)
                price_ok = (level_price <= incoming.price);
            else
                price_ok = (level_price >= incoming.price);

            if (!price_ok)
                break;

            auto &queue_at_price = it->second;
            while (incoming.qty > 0 && !queue_at_price.empty())
            {
                Order &resting = queue_at_price.front();
                uint64_t trade_qty = min(incoming.qty, resting.qty);
                double trade_price = resting.price;

                Trade tr;
                tr.buy_id = (incoming.side == Side::BUY) ? incoming.id : resting.id;
                tr.sell_id = (incoming.side == Side::SELL) ? incoming.id : resting.id;
                tr.qty = trade_qty;
                tr.price = trade_price;
                tr.ts = next_ts++;
                trades.push_back(tr);

                incoming.qty -= trade_qty;
                resting.qty -= trade_qty;

                if (resting.qty == 0)
                    queue_at_price.pop_front();
            }

            if (queue_at_price.empty())
                it = opposite_book.erase(it);
            else
                ++it;
        }

        if (incoming.qty > 0 && incoming.type == Type::IOC)
            incoming.qty = 0;
    }

    void match_order(Order &incoming,
                     map<double, deque<Order>, greater<double>> &opposite_book,
                     map<double, deque<Order>> &own_book,
                     Side opposite_side,
                     vector<Trade> &trades)
    {
        auto it = opposite_book.begin();
        while (incoming.qty > 0 && it != opposite_book.end())
        {
            double level_price = it->first;
            bool price_ok = false;
            if (incoming.type == Type::MARKET)
                price_ok = true;
            else if (incoming.side == Side::BUY)
                price_ok = (level_price <= incoming.price);
            else
                price_ok = (level_price >= incoming.price);

            if (!price_ok)
                break;

            auto &queue_at_price = it->second;
            while (incoming.qty > 0 && !queue_at_price.empty())
            {
                Order &resting = queue_at_price.front();
                uint64_t trade_qty = min(incoming.qty, resting.qty);
                double trade_price = resting.price;

                Trade tr;
                tr.buy_id = (incoming.side == Side::BUY) ? incoming.id : resting.id;
                tr.sell_id = (incoming.side == Side::SELL) ? incoming.id : resting.id;
                tr.qty = trade_qty;
                tr.price = trade_price;
                tr.ts = next_ts++;
                trades.push_back(tr);

                incoming.qty -= trade_qty;
                resting.qty -= trade_qty;

                if (resting.qty == 0)
                    queue_at_price.pop_front();
            }

            if (queue_at_price.empty())
                it = opposite_book.erase(it);
            else
                ++it;
        }

        if (incoming.qty > 0 && incoming.type == Type::IOC)
            incoming.qty = 0;
    }
};

// Generator: produces orders and pushes into TSQueue
void order_generator(TSQueue<Order> &q, size_t orders_to_generate, std::atomic<uint64_t> &global_id, int seed, int thread_idx, double base_price)
{
    mt19937_64 rng(seed + thread_idx);
    uniform_real_distribution<double> price_offset_dist(-1.0, 1.0);
    uniform_int_distribution<int> side_dist(0, 1);
    uniform_int_distribution<int> type_dist(0, 9);
    uniform_int_distribution<int> qty_dist(1, 100);

    for (size_t i = 0; i < orders_to_generate; ++i)
    {
        uint64_t id = ++global_id;
        Side s = side_dist(rng) ? Side::BUY : Side::SELL;
        int tdraw = type_dist(rng);
        Type ty = (tdraw < 1) ? Type::MARKET : (tdraw < 3) ? Type::IOC
                                                           : Type::LIMIT;
        double price = base_price + price_offset_dist(rng);
        uint64_t qty = qty_dist(rng);

        Order o(id, s, ty, price, qty, 0); // timestamp assigned in OrderBook
        q.push(o);
    }
}

int main()
{
    const size_t orders_per_thread = 1000;
    const int num_threads = 4;
    const double base_price = 100.0;

    TSQueue<Order> tsq;
    std::atomic<uint64_t> global_id{0};

    // Launch generator threads
    vector<thread> gen_threads;
    for (int i = 0; i < num_threads; ++i)
    {
        gen_threads.emplace_back(order_generator,
                                 std::ref(tsq),       // pass TSQueue by reference
                                 orders_per_thread,   // copy size_t
                                 std::ref(global_id), // atomic by reference
                                 12345,               // RNG seed
                                 i,                   // thread index (copy)
                                 base_price);         // base price (copy)
    }

    // Order book
    OrderBook ob;

    // Process orders in main thread
    size_t total_orders = orders_per_thread * num_threads;
    size_t orders_processed = 0;

    while (orders_processed < total_orders)
    {
        Order o;
        if (tsq.pop(o))
        {
            vector<Trade> trades = ob.process_order(o);
            // Optional: print trades
            // for (auto &t : trades)
            //     cout << "Trade: " << t.buy_id << " buys from " << t.sell_id
            //          << " " << t.qty << " @ " << t.price << "\n";

            ++orders_processed;
        }
    }

    // Wait for generator threads to finish
    for (auto &t : gen_threads)
        t.join();

    tsq.set_finished();

    // Print top of book
    ob.print_top(5);

    return 0;
}

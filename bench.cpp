#include "OrderBook.hpp"

#include <chrono>
#include <cstdio>
#include <intrin.h>
#include <random>

using namespace lob;
using Clock = std::chrono::steady_clock;

static double NsPer(Clock::time_point a, Clock::time_point b, std::size_t n)
{
    return std::chrono::duration<double, std::nano>(b - a).count() / n;
}

static void Bulk(std::size_t n)
{
    OrderBook ob;
    std::mt19937 rng{ 42 };
    std::uniform_int_distribution<Price> px{ 0, 999 };

    // resting adds: bids 0..999, asks 2000..2999, never cross
    auto t0 = Clock::now();
    for (OrderId id = 1; id <= n; ++id)
    {
        const bool buy = id & 1;
        ob.Submit({ id, buy ? Side::Buy : Side::Sell, buy ? px(rng) : 2000 + px(rng), 10 });
    }
    auto t1 = Clock::now();
    for (OrderId id = 1; id <= n; ++id)
        ob.Cancel(id);
    auto t2 = Clock::now();

    // matching: n resting asks, then n crossing buys (one fill each)
    for (OrderId id = 1; id <= n; ++id)
        ob.Submit({ id, Side::Sell, 2000 + px(rng), 10 });
    std::size_t fills = 0;
    auto t3 = Clock::now();
    for (OrderId id = n + 1; id <= 2 * n; ++id)
        fills += ob.Submit({ id, Side::Buy, 3000, 10 }).size();
    auto t4 = Clock::now();

    std::printf("book=%-7zu add(rest) %6.1f ns/op  cancel %6.1f ns/op  add(match) %6.1f ns/op  fills=%zu\n",
        n, NsPer(t0, t1, n), NsPer(t1, t2, n), NsPer(t3, t4, n), fills);
}

// 55% passive adds, 30% cancels, 10% IOC, 5% market, around a fixed mid.
static void Mixed(std::size_t ops, std::size_t warmBook)
{
    OrderBook ob;
    std::mt19937_64 rng{ 7 };
    std::uniform_int_distribution<int> pct{ 0, 99 };
    std::uniform_int_distribution<Price> off{ 1, 50 };
    std::uniform_int_distribution<Qty> qty{ 1, 100 };
    std::vector<OrderId> live;
    OrderId next = 1;
    constexpr Price mid = 10000;

    auto addPassive = [&] {
        const bool buy = rng() & 1;
        const OrderId id = next++;
        ob.Submit({ id, buy ? Side::Buy : Side::Sell, buy ? mid - off(rng) : mid + off(rng), qty(rng) });
        live.push_back(id);
    };
    for (std::size_t i = 0; i < warmBook; ++i) addPassive();

    std::vector<std::uint64_t> cycles;
    cycles.reserve(ops);
    std::size_t fills = 0;
    const auto start = Clock::now();
    const auto tscStart = __rdtsc();
    for (std::size_t i = 0; i < ops; ++i)
    {
        const int p = pct(rng);
        const auto c0 = __rdtsc();
        if (p < 55)
            addPassive();
        else if (p < 85 && !live.empty())
        {
            std::size_t k = rng() % live.size();
            ob.Cancel(live[k]); // may already be filled: miss path
            live[k] = live.back();
            live.pop_back();
        }
        else if (p < 95)
        {
            const bool buy = rng() & 1;
            fills += ob.Submit({ next++, buy ? Side::Buy : Side::Sell, buy ? mid + 10 : mid - 10, qty(rng), Tif::ImmediateOrCancel }).size();
        }
        else
            fills += ob.Submit({ next++, (rng() & 1) ? Side::Buy : Side::Sell, 0, qty(rng), Tif::Market }).size();
        cycles.push_back(__rdtsc() - c0);
    }
    const auto tscEnd = __rdtsc();
    const auto end = Clock::now();

    const double secs = std::chrono::duration<double>(end - start).count();
    const double nsPerCycle = secs * 1e9 / (tscEnd - tscStart);
    std::sort(cycles.begin(), cycles.end());
    auto q = [&](double f) { return cycles[static_cast<std::size_t>(f * (cycles.size() - 1))] * nsPerCycle; };
    std::printf("mixed %zu ops (warm book %zu): %.2f M ops/s  p50 %.0f ns  p90 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  fills=%zu  final size=%zu\n",
        ops, warmBook, ops / secs / 1e6, q(0.5), q(0.9), q(0.99), q(0.999), fills, ob.Size());
}

int main()
{
    for (std::size_t n : { 1'000, 10'000, 100'000, 1'000'000 })
        Bulk(n);
    Mixed(1'000'000, 1'000);
    Mixed(1'000'000, 10'000);
}

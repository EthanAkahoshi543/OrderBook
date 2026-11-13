#include "OrderBook.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>

using namespace lob;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

static void ExpectBook(const OrderBook& ob, std::size_t orders, std::size_t bidLevels, std::size_t askLevels)
{
    CHECK(ob.Size() == orders);
    CHECK(ob.Depth(Side::Buy).size() == bidLevels);
    CHECK(ob.Depth(Side::Sell).size() == askLevels);
}

// Brute-force model: linear scans, no clever data structures. The real book must agree with it exactly.
struct Reference
{
    struct Resting { OrderId id; Side side; Price price; Qty qty; Tif tif; std::uint64_t seq; };
    std::vector<Resting> book;
    std::vector<Fill> fills;
    std::uint64_t seq = 0;

    static bool Crosses(Side side, Price limit, Price p) { return side == Side::Buy ? p <= limit : p >= limit; }
    static bool Better(Side side, const Resting& a, const Resting& b)
    {
        if (a.price != b.price) return side == Side::Buy ? a.price > b.price : a.price < b.price;
        return a.seq < b.seq;
    }
    Resting* Find(OrderId id) { for (auto& r : book) if (r.id == id) return &r; return nullptr; }
    void Erase(OrderId id) { std::erase_if(book, [&](const Resting& r) { return r.id == id; }); }

    void Submit(const OrderRequest& req)
    {
        fills.clear();
        if (req.qty == 0 || Find(req.id)) return;
        const Side opp = req.side == Side::Buy ? Side::Sell : Side::Buy;
        const Price limit = req.tif != Tif::Market ? req.price
            : req.side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();

        if (req.tif == Tif::FillOrKill)
        {
            std::uint64_t avail = 0;
            for (auto& r : book) if (r.side == opp && Crosses(req.side, limit, r.price)) avail += r.qty;
            if (avail < req.qty) return;
        }

        Qty left = req.qty;
        while (left > 0)
        {
            Resting* best = nullptr;
            for (auto& r : book)
                if (r.side == opp && Crosses(req.side, limit, r.price) && (!best || Better(opp, r, *best)))
                    best = &r;
            if (!best) break;
            const Qty q = std::min(left, best->qty);
            fills.push_back({ req.id, best->id, req.side, best->price, q });
            left -= q;
            if ((best->qty -= q) == 0) Erase(best->id);
        }
        if (left > 0 && (req.tif == Tif::GoodTillCancel || req.tif == Tif::GoodForDay))
            book.push_back({ req.id, req.side, req.price, left, req.tif, seq++ });
    }

    void Replace(OrderId id, Price price, Qty qty)
    {
        fills.clear();
        Resting* r = Find(id);
        if (!r || qty == 0) return;
        if (r->price == price && qty <= r->qty) { r->qty = qty; return; }
        const Resting old = *r;
        Erase(id);
        Submit({ id, old.side, price, qty, old.tif });
    }

    std::vector<DepthLevel> Depth(Side side) const
    {
        std::map<Price, DepthLevel> levels;
        for (auto& r : book)
            if (r.side == side)
            {
                auto& l = levels.try_emplace(r.price, DepthLevel{ r.price, 0, 0 }).first->second;
                l.qty += r.qty;
                ++l.orders;
            }
        std::vector<DepthLevel> out;
        for (auto& [_, l] : levels) out.push_back(l);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());
        return out;
    }
};

static bool Same(std::span<const Fill> a, const std::vector<Fill>& b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); }

static void Fuzz(std::uint64_t seed, int ops)
{
    OrderBook ob{ 64 };
    Reference ref;
    std::mt19937_64 rng{ seed };
    auto pick = [&](int n) { return static_cast<int>(rng() % n); };
    OrderId next = 1;

    for (int i = 0; i < ops; ++i)
    {
        const int op = pick(100);
        const OrderId target = 1 + rng() % (next + 1); // sometimes unknown or already gone
        if (op < 60)
        {
            const OrderRequest req{ pick(10) == 0 ? target : next++, pick(2) ? Side::Buy : Side::Sell,
                100 + pick(11), static_cast<Qty>(pick(20)), static_cast<Tif>(pick(5)) };
            auto fills = ob.Submit(req);
            ref.Submit(req);
            CHECK(Same(fills, ref.fills));
        }
        else if (op < 80)
        {
            CHECK(ob.Cancel(target) == (ref.Find(target) != nullptr));
            ref.Erase(target);
        }
        else if (op < 98)
        {
            const Price price = 100 + pick(11);
            const Qty qty = static_cast<Qty>(pick(20));
            auto fills = ob.Replace(target, price, qty);
            ref.Replace(target, price, qty);
            CHECK(Same(fills, ref.fills));
        }
        else
        {
            ob.ExpireDayOrders();
            std::erase_if(ref.book, [](auto& r) { return r.tif == Tif::GoodForDay; });
        }

        CHECK(ob.Size() == ref.book.size());
        CHECK(ob.Depth(Side::Buy) == ref.Depth(Side::Buy));
        CHECK(ob.Depth(Side::Sell) == ref.Depth(Side::Sell));
        if (ob.BestBid() && ob.BestAsk()) CHECK(*ob.BestBid() < *ob.BestAsk());
    }
}

int main()
{
    using enum Tif;
    using enum Side;

    { // resting orders cross and both fill
        OrderBook ob;
        ob.Submit({ 1, Buy, 100, 10 });
        auto f = ob.Submit({ 2, Sell, 100, 10 });
        CHECK(f.size() == 1 && f[0].maker == 1 && f[0].taker == 2 && f[0].qty == 10);
        ExpectBook(ob, 0, 0, 0);
    }
    { // IOC fully fills
        OrderBook ob;
        ob.Submit({ 1, Buy, 100, 10 });
        CHECK(ob.Submit({ 2, Sell, 100, 10, ImmediateOrCancel }).size() == 1);
        ExpectBook(ob, 0, 0, 0);
    }
    { // IOC remainder is dropped, never rests
        OrderBook ob;
        ob.Submit({ 1, Sell, 100, 4 });
        CHECK(ob.Submit({ 2, Buy, 100, 10, ImmediateOrCancel }).size() == 1);
        ExpectBook(ob, 0, 0, 0);
    }
    { // IOC partial fill while another buy rests at the same price (the original code left the IOC on the book)
        OrderBook ob;
        ob.Submit({ 1, Buy, 99, 5 });
        ob.Submit({ 2, Sell, 99, 3 });
        CHECK(ob.Submit({ 3, Sell, 99, 10, ImmediateOrCancel }).size() == 1);
        CHECK(!ob.Contains(3));
        ExpectBook(ob, 0, 0, 0);
    }
    { // FOK fills when there is enough
        OrderBook ob;
        ob.Submit({ 1, Buy, 100, 10 });
        CHECK(ob.Submit({ 2, Sell, 100, 9, FillOrKill }).size() == 1);
        ExpectBook(ob, 1, 1, 0);
        CHECK(ob.Depth(Buy)[0].qty == 1);
    }
    { // FOK kills when there is not enough, book untouched
        OrderBook ob;
        ob.Submit({ 1, Buy, 100, 10 });
        CHECK(ob.Submit({ 2, Sell, 100, 11, FillOrKill }).empty());
        ExpectBook(ob, 1, 1, 0);
        CHECK(ob.Depth(Buy)[0].qty == 10);
    }
    { // FOK across levels respects its limit price
        OrderBook ob;
        ob.Submit({ 1, Sell, 100, 5 });
        ob.Submit({ 2, Sell, 101, 5 });
        ob.Submit({ 3, Sell, 102, 5 });
        CHECK(ob.Submit({ 4, Buy, 101, 11, FillOrKill }).empty());
        auto f = ob.Submit({ 5, Buy, 101, 10, FillOrKill });
        CHECK(f.size() == 2 && f[0].price == 100 && f[1].price == 101);
        ExpectBook(ob, 1, 0, 1);
    }
    { // market order sweeps every level, remainder dropped
        OrderBook ob;
        for (OrderId id = 1; id <= 10; ++id)
            ob.Submit({ id, Buy, static_cast<Price>(99 + id), 10 });
        auto f = ob.Submit({ 11, Sell, 0, 101, Market });
        CHECK(f.size() == 10 && f[0].price == 109 && f[9].price == 100);
        ExpectBook(ob, 0, 0, 0);
    }
    { // market order into an empty book does nothing
        OrderBook ob;
        CHECK(ob.Submit({ 1, Buy, 0, 10, Market }).empty());
        ExpectBook(ob, 0, 0, 0);
    }
    { // time priority within a level; fills print at the maker's price
        OrderBook ob;
        ob.Submit({ 1, Sell, 100, 3 });
        ob.Submit({ 2, Sell, 100, 3 });
        auto f = ob.Submit({ 3, Buy, 105, 4 });
        CHECK(f.size() == 2 && f[0].maker == 1 && f[0].qty == 3 && f[1].maker == 2 && f[1].qty == 1 && f[1].price == 100);
        CHECK(ob.Depth(Sell)[0].qty == 2);
    }
    { // duplicate ids, zero quantity, unknown cancels are rejected
        OrderBook ob;
        CHECK(ob.Submit({ 1, Buy, 100, 10 }).empty());
        CHECK(ob.Submit({ 1, Buy, 100, 10 }).empty());
        CHECK(ob.Submit({ 2, Buy, 100, 0 }).empty());
        ExpectBook(ob, 1, 1, 0);
        CHECK(ob.Cancel(1));
        CHECK(!ob.Cancel(1));
        CHECK(!ob.Cancel(42));
        ExpectBook(ob, 0, 0, 0);
    }
    { // shrinking keeps priority, moving price loses it
        OrderBook ob;
        ob.Submit({ 1, Buy, 100, 10 });
        ob.Submit({ 2, Buy, 100, 10 });
        CHECK(ob.Replace(1, 100, 4).empty());
        CHECK(ob.Submit({ 3, Sell, 100, 4 })[0].maker == 1);
        ob.Submit({ 4, Buy, 100, 10 });
        ob.Replace(2, 99, 10);
        ob.Replace(2, 100, 10);
        CHECK(ob.Submit({ 5, Sell, 100, 1 })[0].maker == 4);
        CHECK(ob.Replace(42, 1, 1).empty());
    }
    { // replace can cross the spread and trade
        OrderBook ob;
        ob.Submit({ 1, Sell, 101, 5 });
        ob.Submit({ 2, Buy, 99, 5 });
        auto f = ob.Replace(2, 101, 5);
        CHECK(f.size() == 1 && f[0].taker == 2 && f[0].maker == 1);
        ExpectBook(ob, 0, 0, 0);
    }
    { // day orders expire at close, GTC survives
        OrderBook ob;
        ob.Submit({ 1, Sell, 90, 3, GoodForDay });
        ob.Submit({ 2, Sell, 90, 2 });
        ob.Submit({ 3, Buy, 80, 5, GoodForDay });
        CHECK(ob.Depth(Sell)[0].qty == 5 && ob.Depth(Sell)[0].orders == 2);
        CHECK(ob.ExpireDayOrders() == 2);
        ExpectBook(ob, 1, 0, 1);
        CHECK(*ob.BestAsk() == 90 && !ob.BestBid());
    }

    for (std::uint64_t seed = 1; seed <= 200; ++seed)
        Fuzz(seed, 2000);

    std::puts("all tests passed (14 scenarios + 400k randomized ops vs reference model)");
    return 0;
}

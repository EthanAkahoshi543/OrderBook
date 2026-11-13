#pragma once
// Price-time priority limit order book.
// Single-threaded by design: the caller sequences requests (as an exchange's
// matching thread does), so the hot path takes no locks. Orders live in a
// pooled array linked by index, so steady-state operation does not allocate per order.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t; // in ticks
using Qty = std::uint32_t;

enum class Side : std::uint8_t { Buy, Sell };

// Market = ImmediateOrCancel with no price limit.
enum class Tif : std::uint8_t { GoodTillCancel, GoodForDay, ImmediateOrCancel, FillOrKill, Market };

struct OrderRequest
{
    OrderId id;
    Side side;
    Price price; // ignored for Market
    Qty qty;
    Tif tif = Tif::GoodTillCancel;
};

// Executions always print at the resting (maker) order's price.
struct Fill
{
    OrderId taker;
    OrderId maker;
    Side takerSide;
    Price price;
    Qty qty;

    bool operator==(const Fill&) const = default;
};

struct DepthLevel
{
    Price price;
    std::uint64_t qty;
    std::uint32_t orders;

    bool operator==(const DepthLevel&) const = default;
};

class OrderBook
{
public:
    explicit OrderBook(std::size_t expectedOrders = 1 << 16)
    {
        nodes_.reserve(expectedOrders);
        free_.reserve(expectedOrders);
        index_.reserve(expectedOrders);
        fills_.reserve(64);
    }

    // Returned fills stay valid until the next mutating call.
    std::span<const Fill> Submit(const OrderRequest& req)
    {
        fills_.clear();
        if (req.qty == 0 || index_.contains(req.id))
            return {};

        const Price limit = req.tif != Tif::Market ? req.price
            : req.side == Side::Buy ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();

        if (req.tif == Tif::FillOrKill && !CanFill(req.side, limit, req.qty))
            return {};

        const Qty left = Match(req.id, req.side, limit, req.qty);
        if (left > 0 && (req.tif == Tif::GoodTillCancel || req.tif == Tif::GoodForDay))
            Rest(req.id, req.side, req.price, left, req.tif);
        return fills_;
    }

    bool Cancel(OrderId id)
    {
        const auto it = index_.find(id);
        if (it == index_.end())
            return false;
        Remove(it->second);
        index_.erase(it);
        return true;
    }

    // Shrinking quantity at the same price keeps queue priority; anything else re-queues.
    std::span<const Fill> Replace(OrderId id, Price price, Qty qty)
    {
        fills_.clear();
        const auto it = index_.find(id);
        if (it == index_.end() || qty == 0)
            return {};

        Node& node = nodes_[it->second];
        if (price == node.level->first && qty <= node.qty)
        {
            node.level->second.qty -= node.qty - qty;
            node.qty = qty;
            return {};
        }

        const Side side = node.side;
        const Tif tif = node.tif;
        Remove(it->second);
        index_.erase(it);
        return Submit({ id, side, price, qty, tif });
    }

    // Call at session close.
    std::size_t ExpireDayOrders()
    {
        std::size_t expired = 0;
        for (auto it = index_.begin(); it != index_.end();)
        {
            if (nodes_[it->second].tif != Tif::GoodForDay) { ++it; continue; }
            Remove(it->second);
            it = index_.erase(it);
            ++expired;
        }
        return expired;
    }

    std::size_t Size() const { return index_.size(); }
    bool Contains(OrderId id) const { return index_.contains(id); }

    std::optional<Price> BestBid() const { return bids_.empty() ? std::nullopt : std::optional{ bids_.rbegin()->first }; }
    std::optional<Price> BestAsk() const { return asks_.empty() ? std::nullopt : std::optional{ asks_.begin()->first }; }

    // Best price first.
    std::vector<DepthLevel> Depth(Side side) const
    {
        std::vector<DepthLevel> out;
        auto emit = [&](const auto& kv) { out.push_back({ kv.first, kv.second.qty, kv.second.orders }); };
        if (side == Side::Buy)
            std::for_each(bids_.rbegin(), bids_.rend(), emit);
        else
            std::for_each(asks_.begin(), asks_.end(), emit);
        return out;
    }

private:
    static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

    struct Level
    {
        std::uint64_t qty = 0;
        std::uint32_t orders = 0;
        std::uint32_t head = kNil; // oldest order, matched first
        std::uint32_t tail = kNil;
    };

    // Both sides ascending so they share an iterator type; best bid is the last key.
    using Levels = std::map<Price, Level>;

    struct Node
    {
        OrderId id;
        Levels::iterator level; // map iterators stay valid until that level is erased
        Qty qty;
        std::uint32_t prev;
        std::uint32_t next;
        Side side;
        Tif tif;
    };

    Levels bids_;
    Levels asks_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<OrderId, std::uint32_t> index_;
    std::vector<Fill> fills_;

    Levels& Book(Side side) { return side == Side::Buy ? bids_ : asks_; }

    bool CanFill(Side side, Price limit, std::uint64_t need) const
    {
        auto enough = [&](const auto& kv) { if (kv.second.qty >= need) return true; need -= kv.second.qty; return false; };
        if (side == Side::Buy)
        {
            for (auto it = asks_.begin(); it != asks_.end() && it->first <= limit; ++it)
                if (enough(*it)) return true;
        }
        else
        {
            for (auto it = bids_.rbegin(); it != bids_.rend() && it->first >= limit; ++it)
                if (enough(*it)) return true;
        }
        return false;
    }

    Qty Match(OrderId taker, Side side, Price limit, Qty qty)
    {
        Levels& book = side == Side::Buy ? asks_ : bids_;
        while (qty > 0 && !book.empty())
        {
            const auto it = side == Side::Buy ? book.begin() : std::prev(book.end());
            if (side == Side::Buy ? it->first > limit : it->first < limit)
                break;

            Level& level = it->second;
            while (qty > 0 && level.head != kNil)
            {
                const std::uint32_t idx = level.head;
                Node& maker = nodes_[idx];
                const Qty q = std::min(qty, maker.qty);
                fills_.push_back({ taker, maker.id, side, it->first, q });
                qty -= q;
                maker.qty -= q;
                level.qty -= q;
                if (maker.qty == 0)
                {
                    Unlink(idx);
                    index_.erase(maker.id);
                    free_.push_back(idx);
                }
            }
            if (level.head == kNil)
                book.erase(it);
        }
        return qty;
    }

    void Rest(OrderId id, Side side, Price price, Qty qty, Tif tif)
    {
        const auto it = Book(side).try_emplace(price).first;
        std::uint32_t idx;
        if (free_.empty())
        {
            idx = static_cast<std::uint32_t>(nodes_.size());
            nodes_.emplace_back();
        }
        else
        {
            idx = free_.back();
            free_.pop_back();
        }

        Level& level = it->second;
        nodes_[idx] = Node{ id, it, qty, level.tail, kNil, side, tif };
        if (level.tail != kNil)
            nodes_[level.tail].next = idx;
        else
            level.head = idx;
        level.tail = idx;
        ++level.orders;
        level.qty += qty;
        index_.emplace(id, idx);
    }

    void Unlink(std::uint32_t idx)
    {
        const Node& node = nodes_[idx];
        Level& level = node.level->second;
        (node.prev != kNil ? nodes_[node.prev].next : level.head) = node.next;
        (node.next != kNil ? nodes_[node.next].prev : level.tail) = node.prev;
        --level.orders;
    }

    // Caller erases the index entry.
    void Remove(std::uint32_t idx)
    {
        const Node& node = nodes_[idx];
        const auto level = node.level;
        level->second.qty -= node.qty;
        Unlink(idx);
        if (level->second.orders == 0)
            Book(node.side).erase(level);
        free_.push_back(idx);
    }
};

} // namespace lob

//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2020 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_APP_REPORTING_ETLSOURCE_H_INCLUDED
#define RIPPLE_APP_REPORTING_ETLSOURCE_H_INCLUDED

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/websocket.hpp>
#include <backend/BackendInterface.h>
#include <server/SubscriptionManager.h>

#include "org/xrpl/rpc/v1/xrp_ledger.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <etl/ETLHelpers.h>

class ETLLoadBalancer;
class SubscriptionManager;

/// This class manages a connection to a single ETL source. This is almost
/// always a rippled node, but really could be another reporting node. This
/// class subscribes to the ledgers and transactions_proposed streams of the
/// associated rippled node, and keeps track of which ledgers the rippled node
/// has. This class also has methods for extracting said ledgers. Lastly this
/// class forwards transactions received on the transactions_proposed streams to
/// any subscribers.

class ETLSource
{
    std::string ip_;

    std::string wsPort_;

    std::string grpcPort_;

    boost::asio::io_context& ioc_;

    std::unique_ptr<org::xrpl::rpc::v1::XRPLedgerAPIService::Stub> stub_;

    std::unique_ptr<boost::beast::websocket::stream<boost::beast::tcp_stream>>
        ws_;
    boost::asio::ip::tcp::resolver resolver_;

    boost::beast::flat_buffer readBuffer_;

    std::vector<std::pair<uint32_t, uint32_t>> validatedLedgers_;

    std::string validatedLedgersRaw_;

    std::shared_ptr<NetworkValidatedLedgers> networkValidatedLedgers_;

    // beast::Journal journal_;

    mutable std::mutex mtx_;

    size_t numFailures_ = 0;

    std::atomic_bool closing_{false};

    std::atomic_bool connected_{false};

    // true if this ETL source is forwarding transactions received on the
    // transactions_proposed stream. There are usually multiple ETL sources,
    // so to avoid forwarding the same transaction multiple times, we only
    // forward from one particular ETL source at a time.
    std::atomic_bool forwardingStream_{false};

    // The last time a message was received on the ledgers stream
    std::chrono::system_clock::time_point lastMsgTime_;
    mutable std::mutex lastMsgTimeMtx_;

    // used for retrying connections
    boost::asio::steady_timer timer_;

    std::shared_ptr<BackendInterface> backend_;
    std::shared_ptr<SubscriptionManager> subscriptions_;
    ETLLoadBalancer& balancer_;

    void
    run()
    {
        BOOST_LOG_TRIVIAL(trace) << __func__ << " : " << toString();

        auto const host = ip_;
        auto const port = wsPort_;

        resolver_.async_resolve(host, port, [this](auto ec, auto results) {
            onResolve(ec, results);
        });
    }

public:
    static std::unique_ptr<ETLSource>
    make_ETLSource(
        boost::json::object const& config,
        boost::asio::io_context& ioContext,
        std::shared_ptr<BackendInterface> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<NetworkValidatedLedgers> networkValidatedLedgers,
        ETLLoadBalancer& balancer)
    {
        std::unique_ptr<ETLSource> src = std::make_unique<ETLSource>(
            config,
            ioContext,
            backend,
            subscriptions,
            networkValidatedLedgers,
            balancer);

        src->run();

        return src;
    }

    ~ETLSource()
    {
        close(false);
    }

    bool
    isConnected() const
    {
        return connected_;
    }

    std::chrono::system_clock::time_point
    getLastMsgTime() const
    {
        std::lock_guard lck(lastMsgTimeMtx_);
        return lastMsgTime_;
    }

    void
    setLastMsgTime()
    {
        std::lock_guard lck(lastMsgTimeMtx_);
        lastMsgTime_ = std::chrono::system_clock::now();
    }

    /// Create ETL source without gRPC endpoint
    /// Fetch ledger and load initial ledger will fail for this source
    /// Primarly used in read-only mode, to monitor when ledgers are validated
    ETLSource(
        boost::json::object const& config,
        boost::asio::io_context& ioContext,
        std::shared_ptr<BackendInterface> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<NetworkValidatedLedgers> networkValidatedLedgers,
        ETLLoadBalancer& balancer);

    /// @param sequence ledger sequence to check for
    /// @return true if this source has the desired ledger
    bool
    hasLedger(uint32_t sequence) const
    {
        std::lock_guard lck(mtx_);
        for (auto& pair : validatedLedgers_)
        {
            if (sequence >= pair.first && sequence <= pair.second)
            {
                return true;
            }
            else if (sequence < pair.first)
            {
                // validatedLedgers_ is a sorted list of disjoint ranges
                // if the sequence comes before this range, the sequence will
                // come before all subsequent ranges
                return false;
            }
        }
        return false;
    }

    /// process the validated range received on the ledgers stream. set the
    /// appropriate member variable
    /// @param range validated range received on ledgers stream
    void
    setValidatedRange(std::string const& range)
    {
        std::vector<std::pair<uint32_t, uint32_t>> pairs;
        std::vector<std::string> ranges;
        boost::split(ranges, range, boost::is_any_of(","));
        for (auto& pair : ranges)
        {
            std::vector<std::string> minAndMax;

            boost::split(minAndMax, pair, boost::is_any_of("-"));

            if (minAndMax.size() == 1)
            {
                uint32_t sequence = std::stoll(minAndMax[0]);
                pairs.push_back(std::make_pair(sequence, sequence));
            }
            else
            {
                assert(minAndMax.size() == 2);
                uint32_t min = std::stoll(minAndMax[0]);
                uint32_t max = std::stoll(minAndMax[1]);
                pairs.push_back(std::make_pair(min, max));
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](auto left, auto right) {
            return left.first < right.first;
        });

        // we only hold the lock here, to avoid blocking while string processing
        std::lock_guard lck(mtx_);
        validatedLedgers_ = std::move(pairs);
        validatedLedgersRaw_ = range;
    }

    /// @return the validated range of this source
    /// @note this is only used by server_info
    std::string
    getValidatedRange() const
    {
        std::lock_guard lck(mtx_);

        return validatedLedgersRaw_;
    }

    /// Fetch the specified ledger
    /// @param ledgerSequence sequence of the ledger to fetch
    /// @getObjects whether to get the account state diff between this ledger
    /// and the prior one
    /// @return the extracted data and the result status
    std::pair<grpc::Status, org::xrpl::rpc::v1::GetLedgerResponse>
    fetchLedger(uint32_t ledgerSequence, bool getObjects = true);

    std::string
    toString() const
    {
        return "{ validated_ledger : " + getValidatedRange() +
            " , ip : " + ip_ + " , web socket port : " + wsPort_ +
            ", grpc port : " + grpcPort_ + " }";
    }

    boost::json::object
    toJson() const
    {
        boost::json::object res;
        res["validated_range"] = getValidatedRange();
        res["is_connected"] = std::to_string(isConnected());
        res["ip"] = ip_;
        res["ws_port"] = wsPort_;
        res["grpc_port"] = grpcPort_;
        auto last = getLastMsgTime();
        if (last.time_since_epoch().count() != 0)
            res["last_msg_arrival_time"] = std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now() - getLastMsgTime())
                    .count());
        return res;
    }

    /// Download a ledger in full
    /// @param ledgerSequence sequence of the ledger to download
    /// @param writeQueue queue to push downloaded ledger objects
    /// @return true if the download was successful
    bool
    loadInitialLedger(uint32_t ledgerSequence);

    /// Attempt to reconnect to the ETL source
    void
    reconnect(boost::beast::error_code ec);

    /// Callback
    void
    onResolve(
        boost::beast::error_code ec,
        boost::asio::ip::tcp::resolver::results_type results);

    /// Callback
    void
    onConnect(
        boost::beast::error_code ec,
        boost::asio::ip::tcp::resolver::results_type::endpoint_type endpoint);

    /// Callback
    void
    onHandshake(boost::beast::error_code ec);

    /// Callback
    void
    onWrite(boost::beast::error_code ec, size_t size);

    /// Callback
    void
    onRead(boost::beast::error_code ec, size_t size);

    /// Handle the most recently received message
    /// @return true if the message was handled successfully. false on error
    bool
    handleMessage();

    /// Close the websocket
    /// @param startAgain whether to reconnect
    void
    close(bool startAgain);

    /// Get grpc stub to forward requests to rippled node
    /// @return stub to send requests to ETL source
    std::unique_ptr<org::xrpl::rpc::v1::XRPLedgerAPIService::Stub>
    getRippledForwardingStub() const;

    boost::json::object
    forwardToRippled(boost::json::object const& request) const;
};
/// This class is used to manage connections to transaction processing processes
/// This class spawns a listener for each etl source, which listens to messages
/// on the ledgers stream (to keep track of which ledgers have been validated by
/// the network, and the range of ledgers each etl source has). This class also
/// allows requests for ledger data to be load balanced across all possible etl
/// sources.
class ETLLoadBalancer
{
private:
    std::vector<std::unique_ptr<ETLSource>> sources_;

public:
    ETLLoadBalancer(
        boost::json::array const& config,
        boost::asio::io_context& ioContext,
        std::shared_ptr<BackendInterface> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<NetworkValidatedLedgers> nwvl);

    static std::shared_ptr<ETLLoadBalancer>
    make_ETLLoadBalancer(
        boost::json::object const& config,
        boost::asio::io_context& ioc,
        std::shared_ptr<BackendInterface> backend,
        std::shared_ptr<SubscriptionManager> subscriptions,
        std::shared_ptr<NetworkValidatedLedgers> validatedLedgers)
    {
        return std::make_shared<ETLLoadBalancer>(
            config.at("etl_sources").as_array(),
            ioc,
            backend,
            subscriptions,
            validatedLedgers);
    }

    ~ETLLoadBalancer()
    {
        sources_.clear();
    }

    /// Load the initial ledger, writing data to the queue
    /// @param sequence sequence of ledger to download
    /// @param writeQueue queue to push downloaded data to
    void
    loadInitialLedger(uint32_t sequence);

    /// Fetch data for a specific ledger. This function will continuously try
    /// to fetch data for the specified ledger until the fetch succeeds, the
    /// ledger is found in the database, or the server is shutting down.
    /// @param ledgerSequence sequence of ledger to fetch data for
    /// @param getObjects if true, fetch diff between specified ledger and
    /// previous
    /// @return the extracted data, if extraction was successful. If the ledger
    /// was found in the database or the server is shutting down, the optional
    /// will be empty
    std::optional<org::xrpl::rpc::v1::GetLedgerResponse>
    fetchLedger(uint32_t ledgerSequence, bool getObjects);

    /// Determine whether messages received on the transactions_proposed stream
    /// should be forwarded to subscribing clients. The server subscribes to
    /// transactions_proposed on multiple ETLSources, yet only forwards messages
    /// from one source at any given time (to avoid sending duplicate messages
    /// to clients).
    /// @param in ETLSource in question
    /// @return true if messages should be forwarded
    bool
    shouldPropagateTxnStream(ETLSource* in) const
    {
        for (auto& src : sources_)
        {
            assert(src);
            // We pick the first ETLSource encountered that is connected
            if (src->isConnected())
            {
                if (src.get() == in)
                    return true;
                else
                    return false;
            }
        }

        // If no sources connected, then this stream has not been forwarded
        return true;
    }

    boost::json::value
    toJson() const
    {
        boost::json::array ret;
        for (auto& src : sources_)
        {
            ret.push_back(src->toJson());
        }
        return ret;
    }

    /// Randomly select a rippled node to forward a gRPC request to
    /// @return gRPC stub to forward requests to rippled node
    std::unique_ptr<org::xrpl::rpc::v1::XRPLedgerAPIService::Stub>
    getRippledForwardingStub() const;

    /// Forward a JSON RPC request to a randomly selected rippled node
    /// @param request JSON-RPC request
    /// @return response received from rippled node
    boost::json::object
    forwardToRippled(boost::json::object const& request) const;

private:
    /// f is a function that takes an ETLSource as an argument and returns a
    /// bool. Attempt to execute f for one randomly chosen ETLSource that has
    /// the specified ledger. If f returns false, another randomly chosen
    /// ETLSource is used. The process repeats until f returns true.
    /// @param f function to execute. This function takes the ETL source as an
    /// argument, and returns a bool.
    /// @param ledgerSequence f is executed for each ETLSource that has this
    /// ledger
    /// @return true if f was eventually executed successfully. false if the
    /// ledger was found in the database or the server is shutting down
    template <class Func>
    bool
    execute(Func f, uint32_t ledgerSequence);
};

#endif
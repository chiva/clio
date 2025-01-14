//------------------------------------------------------------------------------
/*
    This file is part of clio: https://github.com/XRPLF/clio
    Copyright (c) 2024, the clio developers.

    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#pragma once

#include "data/BackendInterface.h"
#include "rpc/common/Types.h"

namespace rpc {

/**
 * @brief AMMInfoHandler returns information about AMM pools.
 *
 * For more info see: https://xrpl.org/amm_info.html
 */
class AMMInfoHandler {
    std::shared_ptr<BackendInterface> sharedPtrBackend_;

public:
    struct Output {
        // todo: use better type than json types
        boost::json::value amount1;
        boost::json::value amount2;
        boost::json::value lpToken;
        boost::json::array voteSlots;
        boost::json::value auctionSlot;
        std::uint16_t tradingFee = 0;
        std::string ammAccount;
        std::optional<bool> asset1Frozen;
        std::optional<bool> asset2Frozen;

        std::string ledgerHash;
        uint32_t ledgerIndex{};
        bool validated = true;
    };

    struct Input {
        std::optional<ripple::AccountID> accountID;
        std::optional<ripple::AccountID> ammAccount;
        ripple::Issue issue1 = ripple::noIssue();
        ripple::Issue issue2 = ripple::noIssue();
        std::optional<std::string> ledgerHash;
        std::optional<uint32_t> ledgerIndex;
    };

    using Result = HandlerReturnType<Output>;

    AMMInfoHandler(std::shared_ptr<BackendInterface> const& sharedPtrBackend) : sharedPtrBackend_(sharedPtrBackend)
    {
    }

    static RpcSpecConstRef
    spec([[maybe_unused]] uint32_t apiVersion);

    Result
    process(Input input, Context const& ctx) const;

private:
    friend void
    tag_invoke(boost::json::value_from_tag, boost::json::value& jv, Output const& output);

    friend Input
    tag_invoke(boost::json::value_to_tag<Input>, boost::json::value const& jv);
};

}  // namespace rpc

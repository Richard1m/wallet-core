// Copyright © 2017-2020 Trust Wallet.
//
// This file is part of Trust. The full Trust copyright notice, including
// terms governing use, modification, and redistribution, is contained in the
// file LICENSE at the root of the source code distribution tree.

#include "TransactionBuilder.h"
#include "Script.h"
#include "TransactionSigner.h"

#include "../Coin.h"
#include "../proto/Bitcoin.pb.h"

#include <algorithm>
#include <cassert>

namespace TW::Bitcoin {

std::optional<TransactionOutput> TransactionBuilder::prepareOutputWithScript(std::string address, Amount amount, enum TWCoinType coin) {
    auto lockingScript = Script::lockScriptForAddress(address, coin);
    if (lockingScript.empty()) {
        return {};
    }
    return TransactionOutput(amount, lockingScript);
}


/// Estimate encoded size by simple formula
int64_t estimateSimpleFee(const FeeCalculator& feeCalculator, const TransactionPlan& plan, int outputSize, int64_t byteFee) {
    return feeCalculator.calculate(plan.utxos.size(), outputSize, byteFee);
}

/// Estimate encoded size by invoking sign(sizeOnly), get actual size
int64_t estimateSegwitFee(const FeeCalculator& feeCalculator, const TransactionPlan& plan, int outputSize, const SigningInput& input) {
    TWPurpose coinPurpose = TW::purpose(static_cast<TWCoinType>(input.coinType));
    if (coinPurpose != TWPurposeBIP84) {
        // not segwit, return default simple estimate
        return estimateSimpleFee(feeCalculator, plan, outputSize, input.byteFee);
    }

    // duplicate input, with the current plan
    auto inputWithPlan = std::move(input);
    inputWithPlan.plan = plan;

    auto result = TransactionSigner<Transaction, TransactionBuilder>::sign(inputWithPlan, true);
    if (!result) {
        // signing failed; return default simple estimate
        return estimateSimpleFee(feeCalculator, plan, outputSize, input.byteFee);
    }

    // Obtain the encoded size
    auto transaction = result.payload();
    Data dataNonSegwit;
    transaction.encode(dataNonSegwit, Transaction::SegwitFormatMode::NonSegwit);
    int64_t sizeNonSegwit = dataNonSegwit.size();
    uint64_t vSize = 0;
    // Check if there is segwit
    if (!transaction.hasWitness()) {
        // no segwit, virtual size is defined as non-segwit size
        vSize = sizeNonSegwit;
    } else {
        Data dataWitness;
        transaction.encodeWitness(dataWitness);
        int64_t witnessSize = 2 + dataWitness.size();
        // compute virtual size:  (smaller) non-segwit + 1/4 of the diff (witness-only)
        // (in other way: 3/4 of (smaller) non-segwit + 1/4 of segwit size)
        vSize = sizeNonSegwit + witnessSize/4 + (witnessSize % 4 != 0);
    }
    uint64_t fee = input.byteFee * vSize;

    return fee;
}

TransactionPlan TransactionBuilder::plan(const SigningInput& input) {
    TransactionPlan plan;

    const auto& feeCalculator = getFeeCalculator(static_cast<TWCoinType>(input.coinType));
    auto inputSelector = InputSelector<UTXO>(input.utxos, feeCalculator);
    auto inputSum = InputSelector<UTXO>::sum(input.utxos);
    bool maxAmount = input.useMaxAmount;

    if (input.amount == 0 && !maxAmount) {
        plan.error = Common::Proto::Error_zero_amount_requested;
    } else if (input.utxos.empty()) {
        plan.error = Common::Proto::Error_missing_input_utxos;
    } else {
        // select UTXOs
        plan.amount = input.amount;

        // if amount requested is the same or more than available amount, it cannot be satisifed, but
        // treat this case as MaxAmount, and send maximum available (which will be less)
        if (!maxAmount && input.amount >= inputSum) {
            maxAmount = true;
        }

        auto output_size = 2;
        if (!maxAmount) {
            output_size = 2; // output + change
            plan.utxos = inputSelector.select(plan.amount, input.byteFee, output_size);
        } else {
            output_size = 1; // no change
            plan.utxos = inputSelector.selectMaxAmount(input.byteFee);
        }

        if (plan.utxos.size() == 0) {
            plan.amount = 0;
            plan.error = Common::Proto::Error_not_enough_utxos;
        } else {
            plan.availableAmount = InputSelector<UTXO>::sum(plan.utxos);

            // Compute fee.
            // must preliminary set change so that there is a second output
            if (!maxAmount) {
                assert(input.amount <= plan.availableAmount);
                plan.amount = input.amount;
                plan.fee = 0;
                plan.change = plan.availableAmount - plan.amount;
            } else {
                plan.amount = plan.availableAmount;
                plan.fee = 0;
                plan.change = 0;
            }
            plan.fee = estimateSegwitFee(feeCalculator, plan, output_size, input);
            // If fee is larger then availableAmount (can happen in special maxAmount case), we reduce it (and hope it will go through)
            plan.fee = std::min(plan.availableAmount, plan.fee);
            assert(plan.fee >= 0 && plan.fee <= plan.availableAmount);

            // adjust/compute amount
            if (!maxAmount) {
                // reduce amount if needed
                plan.amount = std::max(Amount(0), std::min(plan.amount, plan.availableAmount - plan.fee));
            } else {
                // max available amount
                plan.amount = std::max(Amount(0), plan.availableAmount - plan.fee);
            }
            assert(plan.amount >= 0 && plan.amount <= plan.availableAmount);

            // compute change
            plan.change = plan.availableAmount - plan.amount - plan.fee;
        }
    }
    assert(plan.change >= 0 && plan.change <= plan.availableAmount);
    assert(!maxAmount || plan.change == 0); // change is 0 in max amount case

    assert(plan.amount + plan.change + plan.fee == plan.availableAmount);

    return plan;
}

} // namespace TW::Bitcoin

#include "appcache.h"
#include "main.h"
#include "neuralnet/contract.h"
#include "neuralnet/beacon.h"
#include "neuralnet/project.h"
#include "util.h"

using namespace NN;

// Parses the XML-like elements from contract messages:
std::string ExtractXML(const std::string& XMLdata, const std::string& key, const std::string& key_end);

namespace
{
//!
//! \brief Temporary interface implementation that reads and writes contracts
//! to AppCache to use while we refactor away each of the AppCache sections:
//!
class AppCacheContractHandler : public IContractHandler
{
    void Add(const Contract& contract) override
    {
        WriteCache(
            StringToSection(contract.m_type.ToString()),
            contract.m_key,
            contract.m_value,
            contract.m_tx_timestamp);

        // Update global current poll title displayed in UI:
        // TODO: get rid of this global and make the UI fetch it from the
        // voting contract handler (doesn't exist yet).
        if (contract.m_type == ContractType::POLL) {
            msPoll = contract.ToString();
        }
    }

    void Delete(const Contract& contract) override
    {
        DeleteCache(StringToSection(contract.m_type.ToString()), contract.m_key);
    }
};

//!
//! \brief Handles unknown contract message types by logging a message.
//!
//! After the mandatory switch to version 2 contracts, this class becomes
//! unnecessary--nodes will simply reject transactions with unknown contract
//! types.
//!
class UnknownContractHandler : public IContractHandler
{
    //!
    //! \brief Handle a contract addition.
    //!
    //! \param contract A contract message with an unknown type.
    //!
    void Add(const Contract& contract) override
    {
        contract.Log("WARNING: Add unknown contract type ignored");
    }

    //!
    //! \brief Handle a contract deletion.
    //!
    //! \param contract A contract message with an unknown type.
    //!
    void Delete(const Contract& contract) override
    {
        contract.Log("WARNING: Delete unknown contract type ignored");
    }

    //!
    //! \brief Handle a contract reversal.
    //!
    //! \param contract A contract message with an unknown type.
    //!
    void Revert(const Contract& contract) override
    {
        contract.Log("WARNING: Revert unknown contract type ignored");
    }
};

//!
//! \brief Processes contracts from transaction messages by routing them to the
//! appropriate contract handler implementations.
//!
class Dispatcher
{
public:
    //!
    //! \brief Validate the provided contract and forward it to the appropriate
    //! contract handler.
    //!
    //! \param contract As received from a transaction message.
    //!
    void Apply(const Contract& contract)
    {
        if (contract.m_action == ContractAction::ADD) {
            contract.Log("INFO: Add contract");
            GetHandler(contract.m_type.Value()).Add(contract);
            return;
        }

        if (contract.m_action == ContractAction::REMOVE) {
            contract.Log("INFO: Delete contract");
            GetHandler(contract.m_type.Value()).Delete(contract);
            return;
        }

        contract.Log("WARNING: Unknown contract action ignored");
    }

    //!
    //! \brief Revert a previously-applied contract from a transaction message
    //! by passing it to the appropriate contract handler.
    //!
    //! \param contract Typically parsed from a message in a transaction from
    //! a disconnected block.
    //!
    void Revert(const Contract& contract)
    {
        contract.Log("INFO: Revert contract");

        // The default implementation of IContractHandler reverses an action
        // (addition or deletion) declared in the contract argument, but the
        // type-specific handlers may override this behavior as needed:
        GetHandler(contract.m_type.Value()).Revert(contract);
    }

    //!
    //! \brief Check that the provided contract does not match an existing
    //! contract to protect against replay attacks.
    //!
    //! The application calls this method when it receives a new transaction
    //! from another node. The return value determines whether we should keep
    //! or discard the transaction. If the received contract is too old (as
    //! defined by \c REPLAY_RETENTION_PERIOD), or if the contract matches an
    //! existing contract in the cache, this method returns \c false, and the
    //! calling code shall reject the transaction containing the contract.
    //!
    //! Version 2+ contracts can be checked for replay. Version 1 contracts do
    //! not contain the data necessary to determine uniqueness.
    //!
    //! Replay protection relies on the contract's signing timestamp and nonce
    //! values captured in the contract hash. A contract received with a signing
    //! time earlier than the configured number of seconds from now shall be
    //! considered invalid. The addition of a nonce prevents replay of recent
    //! contracts received within this window.
    //!
    //! \param contract Contract to check. Received in a transaction.
    //!
    //! \return \c true if the check deteremines that the contract is unique.
    //!
    bool CheckReplay(const Contract& contract)
    {
        int64_t valid_after_time = Contract::ReplayPeriod();

        // Reject any contracts timestamped earlier than a reasonable window.
        // By invalidating contracts older than a cut-off threshold, we only
        // need to store contracts newer than REPLAY_RETENTION_PERIOD in memory
        // for replay detection:
        if (contract.m_timestamp < valid_after_time) {
            return false;
        }

        std::list<ReplayPoolItem>::iterator item = m_replay_pool.begin();

        while (item != m_replay_pool.end()) {
            // If a contract hash matches an entry in the pool, we can assume
            // that it was replayed:
            if (item->m_hash == contract.GetHash()) {
                return false;
            }

            // Lazily purge old entries from the pool whenever we check a new
            // contract. Any expired entries eventualy pass out when we receive
            // a valid contract:
            if (item->m_timestamp < valid_after_time) {
                item = m_replay_pool.erase(item);
                continue;
            }

            ++item;
        }

        return true;
    }

    //!
    //! \brief Add a contract to the replay tracking pool.
    //!
    //! \param contract A newly-received contract from a transaction.
    //!
    void TrackForReplay(const Contract& contract)
    {
        ReplayPoolItem new_item;

        new_item.m_timestamp = contract.m_timestamp;
        new_item.m_hash = contract.GetHash();

        m_replay_pool.push_back(std::move(new_item));
    }

private:
    //!
    //! \brief Contains a hash of contract data for replay checks.
    //!
    struct ReplayPoolItem
    {
        int64_t m_timestamp; //!< To cull entries older than retention period.
        uint256 m_hash;      //!< Hash of a contract compared to new contracts.
    };

    //!
    //! \brief Contains a rolling cache of recently-received contract hashes
    //! used to compare with contracts received in transaction messages for
    //! replay protection.
    //!
    //! Calling \c CheckReplay() will purge the old entries from the cache as
    //! it checks a valid contract.
    //!
    std::list<ReplayPoolItem> m_replay_pool;

    AppCacheContractHandler m_appcache_handler; //<! Temporary.
    UnknownContractHandler m_unknown_handler;   //<! Logs unknown types.

    //!
    //! \brief Select an appropriate contract handler based on the message type.
    //!
    //! \param type Contract type. Determines how to handle the message.
    //!
    //! \return Reference to an object capable of handling the contract type.
    //!
    IContractHandler& GetHandler(const ContractType type)
    {
        // TODO: build contract handlers for the remaining contract types:
        // TODO: refactor to dynamic registration for easier testing:
        switch (type) {
            case ContractType::BEACON:     return GetBeaconDirectory();
            case ContractType::POLL:       return m_appcache_handler;
            case ContractType::PROJECT:    return GetWhitelist();
            case ContractType::PROTOCOL:   return m_appcache_handler;
            case ContractType::SCRAPER:    return m_appcache_handler;
            //case ContractType::SUPERBLOCK: // currently handled elsewhere
            case ContractType::VOTE:       return m_appcache_handler;
            default:                       return m_unknown_handler;
        }
    }
}; // class Dispatcher

//!
//! \brief Global contract dispatcher instance.
//!
Dispatcher g_dispatcher;

} // anonymous namespace

// -----------------------------------------------------------------------------
// Global Functions
// -----------------------------------------------------------------------------

void NN::ProcessContract(const Contract& contract)
{
    g_dispatcher.Apply(contract);
}

void NN::RevertContract(const Contract& contract)
{
    g_dispatcher.Revert(contract);
}

bool NN::CheckContractReplay(const Contract& contract)
{
    return gateway.CheckReplay(contract);
}

void NN::TrackContracts(const std::vector<Contract>& contracts)
{
    for (const auto& contract : contracts) {
        gateway.TrackForReplay(contract);
    }
}

// -----------------------------------------------------------------------------
// Class: Contract
// -----------------------------------------------------------------------------

Contract::Contract()
    : m_version(Contract::CURRENT_VERSION)
    , m_type(Contract::Type(ContractType::UNKNOWN))
    , m_action(Contract::Action(ContractAction::UNKNOWN))
    , m_key(std::string())
    , m_value(std::string())
    , m_signature(Contract::Signature())
    , m_public_key(Contract::PublicKey())
    , m_nonce(0)
    , m_timestamp(0)
    , m_tx_timestamp(0)
{
}

Contract::Contract(
    Contract::Type type,
    Contract::Action action,
    std::string key,
    std::string value)
    : m_version(Contract::CURRENT_VERSION)
    , m_type(std::move(type))
    , m_action(std::move(action))
    , m_key(std::move(key))
    , m_value(std::move(value))
    , m_signature(Contract::Signature())
    , m_public_key(Contract::PublicKey())
    , m_nonce(0)
    , m_timestamp(0)
    , m_tx_timestamp(0)
{
}

Contract::Contract(
    int version,
    Contract::Type type,
    Contract::Action action,
    std::string key,
    std::string value,
    Contract::Signature signature,
    Contract::PublicKey public_key,
    unsigned int nonce,
    long timestamp,
    int64_t tx_timestamp)
    : m_version(version)
    , m_type(std::move(type))
    , m_action(std::move(action))
    , m_key(std::move(key))
    , m_value(std::move(value))
    , m_signature(std::move(signature))
    , m_public_key(std::move(public_key))
    , m_nonce(std::move(nonce))
    , m_timestamp(std::move(timestamp))
    , m_tx_timestamp(std::move(tx_timestamp))
{
}

const CPubKey& Contract::MasterPublicKey()
{
    // If the master key changes, add a conditional entry to this method that
    // returns the new key for the appropriate height.

    // 049ac003b3318d9fe28b2830f6a95a2624ce2a69fb0c0c7ac0b513efcc1e93a6a
    // 6e8eba84481155dd82f2f1104e0ff62c69d662b0094639b7106abc5d84f948c0a
    static const CPubKey since_block_0(std::vector<unsigned char> {
        0x04, 0x9a, 0xc0, 0x03, 0xb3, 0x31, 0x8d, 0x9f, 0xe2, 0x8b, 0x28,
        0x30, 0xf6, 0xa9, 0x5a, 0x26, 0x24, 0xce, 0x2a, 0x69, 0xfb, 0x0c,
        0x0c, 0x7a, 0xc0, 0xb5, 0x13, 0xef, 0xcc, 0x1e, 0x93, 0xa6, 0xa6,
        0xe8, 0xeb, 0xa8, 0x44, 0x81, 0x15, 0x5d, 0xd8, 0x2f, 0x2f, 0x11,
        0x04, 0xe0, 0xff, 0x62, 0xc6, 0x9d, 0x66, 0x2b, 0x00, 0x94, 0x63,
        0x9b, 0x71, 0x06, 0xab, 0xc5, 0xd8, 0x4f, 0x94, 0x8c, 0x0a
    });

    return since_block_0;
}

const CPrivKey Contract::MasterPrivateKey()
{
    std::vector<unsigned char> key = ParseHex(GetArgument("masterprojectkey", ""));

    return CPrivKey(key.begin(), key.end());
}

const CPubKey& Contract::MessagePublicKey()
{
    // If the message key changes, add a conditional entry to this method that
    // returns the new key for the appropriate height.

    // 044b2938fbc38071f24bede21e838a0758a52a0085f2e034e7f971df445436a25
    // 2467f692ec9c5ba7e5eaa898ab99cbd9949496f7e3cafbf56304b1cc2e5bdf06e
    static const CPubKey since_block_0 ({
        0x04, 0x4b, 0x29, 0x38, 0xfb, 0xc3, 0x80, 0x71, 0xf2, 0x4b, 0xed,
        0xe2, 0x1e, 0x83, 0x8a, 0x07, 0x58, 0xa5, 0x2a, 0x00, 0x85, 0xf2,
        0xe0, 0x34, 0xe7, 0xf9, 0x71, 0xdf, 0x44, 0x54, 0x36, 0xa2, 0x52,
        0x46, 0x7f, 0x69, 0x2e, 0xc9, 0xc5, 0xba, 0x7e, 0x5e, 0xaa, 0x89,
        0x8a, 0xb9, 0x9c, 0xbd, 0x99, 0x49, 0x49, 0x6f, 0x7e, 0x3c, 0xaf,
        0xbf, 0x56, 0x30, 0x4b, 0x1c, 0xc2, 0xe5, 0xbd, 0xf0, 0x6e
    });

    return since_block_0;
}

const CPrivKey& Contract::MessagePrivateKey()
{
    // If the message key changes, add a conditional entry to this method that
    // returns the new key for the appropriate height.

    // 308201130201010420fbd45ffb02ff05a3322c0d77e1e7aea264866c24e81e5ab
    // 6a8e150666b4dc6d8a081a53081a2020101302c06072a8648ce3d0101022100ff
    // fffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f300
    // 604010004010704410479be667ef9dcbbac55a06295ce870b07029bfcdb2dce28
    // d959f2815b16f81798483ada7726a3c4655da4fbfc0e1108a8fd17b448a685541
    // 99c47d08ffb10d4b8022100fffffffffffffffffffffffffffffffebaaedce6af
    // 48a03bbfd25e8cd0364141020101a144034200044b2938fbc38071f24bede21e8
    // 38a0758a52a0085f2e034e7f971df445436a252467f692ec9c5ba7e5eaa898ab9
    // 9cbd9949496f7e3cafbf56304b1cc2e5bdf06e
    static const CPrivKey since_block_0 {
        0x30, 0x82, 0x01, 0x13, 0x02, 0x01, 0x01, 0x04, 0x20, 0xfb, 0xd4,
        0x5f, 0xfb, 0x02, 0xff, 0x05, 0xa3, 0x32, 0x2c, 0x0d, 0x77, 0xe1,
        0xe7, 0xae, 0xa2, 0x64, 0x86, 0x6c, 0x24, 0xe8, 0x1e, 0x5a, 0xb6,
        0xa8, 0xe1, 0x50, 0x66, 0x6b, 0x4d, 0xc6, 0xd8, 0xa0, 0x81, 0xa5,
        0x30, 0x81, 0xa2, 0x02, 0x01, 0x01, 0x30, 0x2c, 0x06, 0x07, 0x2a,
        0x86, 0x48, 0xce, 0x3d, 0x01, 0x01, 0x02, 0x21, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xfc, 0x2f, 0x30, 0x06, 0x04,
        0x01, 0x00, 0x04, 0x01, 0x07, 0x04, 0x41, 0x04, 0x79, 0xbe, 0x66,
        0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0, 0x62, 0x95, 0xce, 0x87,
        0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d, 0xce, 0x28, 0xd9, 0x59,
        0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98, 0x48, 0x3a, 0xda, 0x77,
        0x26, 0xa3, 0xc4, 0x65, 0x5d, 0xa4, 0xfb, 0xfc, 0x0e, 0x11, 0x08,
        0xa8, 0xfd, 0x17, 0xb4, 0x48, 0xa6, 0x85, 0x54, 0x19, 0x9c, 0x47,
        0xd0, 0x8f, 0xfb, 0x10, 0xd4, 0xb8, 0x02, 0x21, 0x00, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xfe, 0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
        0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41, 0x02, 0x01, 0x01,
        0xa1, 0x44, 0x03, 0x42, 0x00, 0x04, 0x4b, 0x29, 0x38, 0xfb, 0xc3,
        0x80, 0x71, 0xf2, 0x4b, 0xed, 0xe2, 0x1e, 0x83, 0x8a, 0x07, 0x58,
        0xa5, 0x2a, 0x00, 0x85, 0xf2, 0xe0, 0x34, 0xe7, 0xf9, 0x71, 0xdf,
        0x44, 0x54, 0x36, 0xa2, 0x52, 0x46, 0x7f, 0x69, 0x2e, 0xc9, 0xc5,
        0xba, 0x7e, 0x5e, 0xaa, 0x89, 0x8a, 0xb9, 0x9c, 0xbd, 0x99, 0x49,
        0x49, 0x6f, 0x7e, 0x3c, 0xaf, 0xbf, 0x56, 0x30, 0x4b, 0x1c, 0xc2,
        0xe5, 0xbd, 0xf0, 0x6e
    };

    return since_block_0;
}

const std::string Contract::BurnAddress()
{
    return fTestNet
        ? "mk1e432zWKH1MW57ragKywuXaWAtHy1AHZ"
        : "S67nL4vELWwdDVzjgtEP4MxryarTZ9a8GB";
}

int64_t Contract::ReplayPeriod()
{
    return GetAdjustedTime() - REPLAY_RETENTION_PERIOD;
}

bool Contract::Detect(const std::string& message)
{
    return !message.empty()
        && Contains(message, "<MT>")
        // Superblock currently handled elsewhere:
        && !Contains(message, "<MT>superblock</MT>");
}

Contract Contract::Parse(const std::string& message, const int64_t timestamp)
{
    if (message.empty()) {
        return Contract();
    }

    return Contract(
        1, // Legacy XML-like string contracts always parse to a v1 contract.
        Contract::Type::Parse(ExtractXML(message, "<MT>", "</MT>")),
        Contract::Action::Parse(ExtractXML(message, "<MA>", "</MA>")),
        ExtractXML(message, "<MK>", "</MK>"),
        ExtractXML(message, "<MV>", "</MV>"),
        Contract::Signature::Parse(ExtractXML(message, "<MS>", "</MS>")),
        // None of the currently-valid contract types support signing with a
        // user-supplied private key, so we can skip parsing the public keys
        // altogether. We verify contracts with the master and message keys:
        //Contract::PublicKey::Parse(ExtractXML(message, "<MPK>", "</MPK>")),
        Contract::PublicKey(),
        0, // Nonce unused in v1 contracts.
        0, // Signing timestamp unused in v1 contracts.
        timestamp);
}

bool Contract::RequiresMasterKey() const
{
    switch (m_type.Value()) {
        case ContractType::BEACON:   return m_action == ContractAction::REMOVE;
        case ContractType::POLL:     return m_action == ContractAction::REMOVE;
        case ContractType::PROJECT:  return true;
        case ContractType::PROTOCOL: return true;
        case ContractType::SCRAPER:  return true;
        case ContractType::VOTE:     return m_action == ContractAction::REMOVE;
        default:                     return false;
    }
}

bool Contract::RequiresMessageKey() const
{
    switch (m_type.Value()) {
        case ContractType::BEACON: return m_action == ContractAction::ADD;
        case ContractType::POLL:   return m_action == ContractAction::ADD;
        case ContractType::VOTE:   return m_action == ContractAction::ADD;
        default:                   return false;
    }
}

bool Contract::RequiresSpecialKey() const
{
    return RequiresMessageKey() || RequiresMasterKey();
}

const CPubKey& Contract::ResolvePublicKey() const
{
    if (RequiresMessageKey()) {
        return MessagePublicKey();
    }

    if (RequiresMasterKey()) {
        return MasterPublicKey();
    }

    return m_public_key.Key();
}

bool Contract::WellFormed() const
{
    return m_version > 0 && m_version <= Contract::CURRENT_VERSION
        && m_type != ContractType::UNKNOWN
        && m_action != ContractAction::UNKNOWN
        && !m_key.empty()
        && !m_value.empty()
        && m_signature.Viable()
        && (RequiresSpecialKey() || m_public_key.Viable())
        && m_tx_timestamp > 0
        && (m_version == 1 || (m_timestamp > 0 && m_nonce > 0));
}

bool Contract::Validate() const
{
    return WellFormed() && VerifySignature();
}

bool Contract::Sign(CKey& private_key)
{
    std::vector<unsigned char> output;
    m_hash_cache = 0; // Invalidate the cached hash, if any.

    if (m_version > 1) {
        m_timestamp = GetAdjustedTime();
        RAND_bytes(reinterpret_cast<unsigned char*>(&m_nonce), sizeof(m_nonce));
    }

    if (!private_key.Sign(GetHash(), output)) {
        Log("ERROR: Failed to sign contract");
        return false;
    }

    m_signature = std::move(output);

    if (!RequiresSpecialKey()) {
        m_public_key = private_key.GetPubKey();
    }

    return true;
}

bool Contract::SignWithMessageKey()
{
    CKey key;

    key.SetPrivKey(MessagePrivateKey());

    return Sign(key);
}

bool Contract::VerifySignature() const
{
    CKey key;

    if (!key.SetPubKey(ResolvePublicKey())) {
        Log("ERROR: Failed to set contract public key");
        return false;
    }

    return key.Verify(GetHash(), m_signature.Raw());
}

uint256 Contract::GetHash() const
{
    // Nodes use the hash at least twice when validating a received contract
    // (once to verify the signature, once to track the contract for replay
    // protection, and one or more times to compare the contract to any other
    // contracts in the replay protection cache), so we cache the value to
    // avoid re-computing it. Once cached, the hash is only invalidated when
    // re-signing a contract, so avoid calling this method on a contract in
    // an intermediate state.

    if (m_hash_cache > 0) {
        return m_hash_cache;
    }

    if (m_version > 1) {
        m_hash_cache = SerializeHash(*this);
        return m_hash_cache;
    }

    const std::string type_string = m_type.ToString();

    m_hash_cache = Hash(
        type_string.begin(),
        type_string.end(),
        m_key.begin(),
        m_key.end(),
        m_value.begin(),
        m_value.end());

    return m_hash_cache;
}

std::string Contract::ToString() const
{
    return "<MT>" + m_type.ToString()       + "</MT>"
        + "<MK>"  + m_key                   + "</MK>"
        + "<MV>"  + m_value                 + "</MV>"
        + "<MA>"  + m_action.ToString()     + "</MA>"
        + "<MPK>" + m_public_key.ToString() + "</MPK>"
        + "<MS>"  + m_signature.ToString()  + "</MS>";
}

void Contract::Log(const std::string& prefix) const
{
    // TODO: temporary... needs better logging
    if (!fDebug) {
        return;
    }

    LogPrintf(
        "<Contract::Log>: %s: v%d, %d, %d, %s, %s, %s, %s, %s, %s, %d",
        prefix,
        m_version,
        m_tx_timestamp,
        m_timestamp,
        m_type.ToString(),
        m_action.ToString(),
        m_key,
        m_value,
        m_public_key.ToString(),
        m_signature.ToString(),
        m_nonce);
}

// -----------------------------------------------------------------------------
// Class: Contract::Type
// -----------------------------------------------------------------------------

Contract::Type::Type(ContractType type)
    : EnumVariant(type, boost::none)
{
}

Contract::Type::Type(std::string other)
    : EnumVariant(ContractType::UNKNOWN, std::move(other))
{
}

Contract::Type::Type(ContractType type, std::string other)
    : EnumVariant(type, std::move(other))
{
}

Contract::Type Contract::Type::Parse(std::string input)
{
    if (input.empty())             return ContractType::UNKNOWN;

    // Ordered by frequency:
    if (input == "beacon")         return ContractType::BEACON;
    if (input == "vote")           return ContractType::VOTE;
    if (input == "poll")           return ContractType::POLL;
    if (input == "project")        return ContractType::PROJECT;
    if (input == "scraper")        return ContractType::SCRAPER;
    if (input == "protocol")       return ContractType::PROTOCOL;

    // Currently handled elsewhere:
    if (input == "superblock")     return ContractType::SUPERBLOCK;

    // Legacy type for "project" (found at height 267504, 410257):
    if (input == "projectmapping") {
        return Contract::Type(ContractType::PROJECT, "projectmapping");
    }

    return Contract::Type(std::move(input));
}

std::string Contract::Type::ToString() const
{
    if (const EnumVariant::OptionalString other = m_other) {
        return *other;
    }

    switch (m_value) {
        case ContractType::BEACON:     return "beacon";
        case ContractType::POLL:       return "poll";
        case ContractType::PROJECT:    return "project";
        case ContractType::PROTOCOL:   return "protocol";
        case ContractType::SCRAPER:    return "scraper";
        case ContractType::SUPERBLOCK: return "superblock";
        case ContractType::VOTE:       return "vote";
        default:                       return "";
    }
}

// -----------------------------------------------------------------------------
// Class: Contract::Action
// -----------------------------------------------------------------------------

Contract::Action::Action(ContractAction action)
    : EnumVariant(action, boost::none)
{
}

Contract::Action::Action(std::string other)
    : EnumVariant(ContractAction::UNKNOWN, std::move(other))
{
}

Contract::Action Contract::Action::Parse(std::string input)
{
    if (input.empty()) return ContractAction::UNKNOWN;
    if (input == "A")  return ContractAction::ADD;
    if (input == "D")  return ContractAction::REMOVE;

    return Contract::Action(std::move(input));
}

std::string Contract::Action::ToString() const
{
    if (const EnumVariant::OptionalString other = m_other) {
        return *other;
    }

    switch (m_value) {
        case ContractAction::ADD:    return "A";
        case ContractAction::REMOVE: return "D";
        default:                     return "";
    }
}

// -----------------------------------------------------------------------------
// Class: Contract::Signature
// -----------------------------------------------------------------------------

Contract::Signature::Signature() : m_bytes(std::vector<unsigned char>(0))
{
}

Contract::Signature::Signature(std::vector<unsigned char> bytes)
    : m_bytes(std::move(bytes))
{
}

Contract::Signature Contract::Signature::Parse(const std::string& input)
{
    if (input.empty()) {
        return Contract::Signature();
    }

    bool invalid;
    std::vector<unsigned char> decoded = DecodeBase64(input.c_str(), &invalid);

    if (invalid) {
        return Contract::Signature();
    }

    return Contract::Signature(std::move(decoded));
}

bool Contract::Signature::Viable() const
{
    // The DER-encoded ASN.1 ECDSA signatures typically contain 70 or 71 bytes,
    // but may hold up to 73. Sizes as low as 68 bytes seen on mainnet. We only
    // check the number of bytes here as an early step:
    return m_bytes.size() >= 64 && m_bytes.size() <= 73;
}

const std::vector<unsigned char>& Contract::Signature::Raw() const
{
    return m_bytes;
}

std::string Contract::Signature::ToString() const
{
    if (m_bytes.empty()) {
        return std::string();
    }

    return EncodeBase64(m_bytes.data(), m_bytes.size());
}

// -----------------------------------------------------------------------------
// Class: Contract::PublicKey
// -----------------------------------------------------------------------------

Contract::PublicKey::PublicKey() : m_key(CPubKey())
{
}

Contract::PublicKey::PublicKey(CPubKey key)
    : m_key(std::move(key))
{
}

Contract::PublicKey Contract::PublicKey::Parse(const std::string& input)
{
    if (input.empty()) {
        return Contract::PublicKey();
    }

    return Contract::PublicKey(CPubKey(ParseHex(input)));
}

bool Contract::PublicKey::operator==(const CPubKey& other) const
{
    return m_key == other;
}

bool Contract::PublicKey::operator!=(const CPubKey& other) const
{
    return m_key != other;
}

bool Contract::PublicKey::Viable() const
{
    return m_key.IsValid();
}

const CPubKey& Contract::PublicKey::Key() const
{
    return m_key;
}

std::string Contract::PublicKey::ToString() const
{
    return HexStr(m_key.Raw());
}

// -----------------------------------------------------------------------------
// Abstract Class: IContractHandler
// -----------------------------------------------------------------------------

void IContractHandler::Revert(const Contract& contract)
{
    if (contract.m_action == ContractAction::ADD) {
        Delete(contract);
        return;
    }

    if (contract.m_action == ContractAction::REMOVE) {
        Add(contract);
        return;
    }

    error("Unknown contract action ignored: %s", contract.m_action.ToString());
}

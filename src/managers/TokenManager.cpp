#include "TokenManager.hpp"
#ifdef __ANDROID__
#include <sys/random.h>
#include <array>
#include <cerrno>
#include <stdexcept>
#else
#include <uuid/uuid.h>
#endif
#include <algorithm>

CUUIDToken::CUUIDToken(const std::string& uuid_, std::any data_, Time::steady_dur expires) : m_data(data_), m_uuid(uuid_) {
    m_expiresAt = Time::steadyNow() + expires;
}

std::string CUUIDToken::getUUID() {
    return m_uuid;
}

std::string CTokenManager::getRandomUUID() {
    std::string uuid;
    do {
#ifdef __ANDROID__
        std::array<unsigned char, 16> uuid_{};
        size_t                        offset = 0;
        while (offset < uuid_.size()) {
            const auto count = getrandom(uuid_.data() + offset, uuid_.size() - offset, 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                throw std::runtime_error("Cannot obtain random bytes for a UUID");
            offset += count;
        }
        uuid_[6] = (uuid_[6] & 0x0f) | 0x40;
        uuid_[8] = (uuid_[8] & 0x3f) | 0x80;
#else
        uuid_t uuid_;
        uuid_generate_random(uuid_);
#endif
        uuid = std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", sc<uint16_t>(uuid_[0]), sc<uint16_t>(uuid_[1]),
                           sc<uint16_t>(uuid_[2]), sc<uint16_t>(uuid_[3]), sc<uint16_t>(uuid_[4]), sc<uint16_t>(uuid_[5]), sc<uint16_t>(uuid_[6]), sc<uint16_t>(uuid_[7]),
                           sc<uint16_t>(uuid_[8]), sc<uint16_t>(uuid_[9]), sc<uint16_t>(uuid_[10]), sc<uint16_t>(uuid_[11]), sc<uint16_t>(uuid_[12]), sc<uint16_t>(uuid_[13]),
                           sc<uint16_t>(uuid_[14]), sc<uint16_t>(uuid_[15]));
    } while (m_tokens.contains(uuid));

    return uuid;
}

std::string CTokenManager::registerNewToken(std::any data, Time::steady_dur expires) {
    std::string uuid = getRandomUUID();

    m_tokens[uuid] = makeShared<CUUIDToken>(uuid, data, expires);
    return uuid;
}

SP<CUUIDToken> CTokenManager::getToken(const std::string& uuid) {

    // cleanup expired tokens
    const auto NOW = Time::steadyNow();
    std::erase_if(m_tokens, [&NOW](const auto& el) { return el.second->m_expiresAt < NOW; });

    if (!m_tokens.contains(uuid))
        return {};

    return m_tokens.at(uuid);
}

void CTokenManager::removeToken(SP<CUUIDToken> token) {
    if (!token)
        return;
    m_tokens.erase(token->m_uuid);
}

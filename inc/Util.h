#ifndef MESSAGING_UTIL_HPP
#define MESSAGING_UTIL_HPP

#include <stdexcept>
#include <core/MojObject.h>
#include <core/MojString.h>
#include <core/MojJson.h>

#include <purple.h>

namespace Util
{

    MojString get (MojObject const& obj, const char* key);

    struct MojoException
    {
        MojoException (std::string const& err) : err_(err)
        {}

        virtual ~MojoException () {}

        std::string const& what() const { return err_; }

        std::string err_;
    };

    MojObject getProtocolOptions(MojString prpl);
    PurpleAccount* createPurpleAccount(const char* username, const char* prpl_id, MojObject config);
    // Set each of the account's protocol options that prefs holds (createPurpleAccount does this
    // for a new account; this is the same for one that exists already).
    void applyProtocolOptions(PurpleAccount* account, const MojObject& prefs);

}

#endif

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

}

#endif

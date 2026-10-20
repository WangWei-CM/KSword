#pragma once
#include "PrivilegeLegacyReadService.h"
#include <utility>

namespace ks::privilege::legacy
{
    class PasswordValue final
    {
    public:
        explicit PasswordValue(std::wstring text) : m_text(std::move(text)) {}
        PasswordValue(const PasswordValue&) = delete;
        PasswordValue& operator=(const PasswordValue&) = delete;
        ~PasswordValue() { clear(); }
        const std::wstring& value() const { return m_text; }
        void clear() noexcept
        {
            volatile wchar_t* data = m_text.empty() ? nullptr : m_text.data();
            for (std::size_t index = 0; index < m_text.size(); ++index) data[index] = L'\0';
            m_text.clear();
        }
    private:
        std::wstring m_text;
    };
    enum class MutationKind { CreateUser, ResetPassword };
    struct MutationRequest
    {
        MutationKind kind = MutationKind::CreateUser;
        QString userName;
        std::shared_ptr<PasswordValue> password;
        UserIdentity expectedIdentity;
    };
    struct MutationReceipt
    {
        WriteStatus status;
        bool writeAttempted = false;
        bool outcomeKnown = true;
        bool identityChanged = false;
        bool readbackFailed = false;
        std::shared_ptr<ReadSnapshot> after;
        bool succeeded() const { return writeAttempted && outcomeKnown && status.code == Success; }
    };
    struct MutationPorts
    {
        std::function<UserIdentity(const QString&)> identity;
        std::function<WriteStatus(const QString&, const std::wstring&)> create;
        std::function<WriteStatus(const QString&, const std::wstring&)> reset;
        std::function<ReadSnapshot()> readback;
    };
    // Accepted mutations are executed once. Cancellation belongs before acceptance only.
    inline MutationReceipt executeMutation(const MutationRequest& request, const MutationPorts& ports)
    {
        MutationReceipt receipt;
        struct ErasePassword
        {
            std::shared_ptr<PasswordValue> password;
            ~ErasePassword() { if (password) password->clear(); }
        } erase{request.password};
        try
        {
            bool permitted = request.password && !request.password->value().empty() && !request.userName.isEmpty();
            if (!permitted) receipt.status.code = 87; // ERROR_INVALID_PARAMETER
            if (permitted && request.kind == MutationKind::ResetPassword)
            {
                const UserIdentity current = ports.identity(request.userName);
                permitted = request.expectedIdentity.valid && current.valid
                    && request.expectedIdentity.code == Success && current.code == Success
                    && current.sid == request.expectedIdentity.sid
                    && current.name.compare(request.expectedIdentity.name, Qt::CaseInsensitive) == 0
                    && current.name.compare(request.userName, Qt::CaseInsensitive) == 0;
                if (!permitted)
                {
                    // Query failures retain their native status for the existing elevation recovery UI.
                    receipt.identityChanged = current.code == Success;
                    receipt.status.code = current.code != Success ? current.code : 1307; // ERROR_INVALID_OWNER
                }
            }
            if (permitted)
            {
                receipt.writeAttempted = true;
                receipt.status = request.kind == MutationKind::CreateUser
                    ? ports.create(request.userName, request.password->value())
                    : ports.reset(request.userName, request.password->value());
            }
        }
        catch (...)
        {
            // A transport that throws after accepting a write has an unknown outcome; never retry it.
            receipt.outcomeKnown = !receipt.writeAttempted;
            receipt.status.code = 574; // ERROR_UNHANDLED_EXCEPTION
        }
        if (request.password) request.password->clear();
        try
        {
            receipt.after = std::make_shared<ReadSnapshot>(ports.readback());
        }
        catch (...)
        {
            receipt.readbackFailed = true;
        }
        return receipt;
    }
    inline MutationReceipt executeNativeMutation(const MutationRequest& request)
    {
        return executeMutation(request, {queryNativeIdentity, createNativeUser, resetNativePassword,
            []() { return queryNative({All, 65536}); }});
    }
}

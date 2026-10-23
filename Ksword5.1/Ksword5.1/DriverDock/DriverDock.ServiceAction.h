#pragma once

// Value-only request/receipt and adapter seam. No UI, real SCM or filesystem is
// required by tests. The caller must dispatch accepted writes as transactions.
#include "../ksword/service/service.h"
#include <cstdint>
#include <exception>
#include <string>

namespace ksword::driver_service_action
{
    struct RegistrationRequest
    {
        ks::service::KernelDriverServiceConfig config;
        std::wstring unquotedPath;
        bool systemRootPath = false;
        std::uint64_t formRevision = 0;
    };

    struct RegistrationReceipt
    {
        RegistrationRequest request;
        bool pathUnavailable = false;
        bool succeeded = false;
        bool created = false;
        std::string errorText;
        std::uint32_t errorCode = 0;
        bool readbackSucceeded = false;
        ks::service::ServiceStatus status{};
        std::string readbackErrorText;
        std::uint32_t readbackErrorCode = 0;
    };

    template<class PathExists, class Mutate, class QueryStatus>
    RegistrationReceipt RunRegistration(const RegistrationRequest& request,
        PathExists pathExists, Mutate mutate, QueryStatus queryStatus)
    {
        RegistrationReceipt receipt;
        receipt.request = request;
        // This warning has never been a veto: preserve the original attempt.
        receipt.pathUnavailable = !request.systemRootPath && !pathExists(request.unquotedPath);
        receipt.succeeded = mutate(request.config, &receipt.created,
            &receipt.errorText, &receipt.errorCode);
        if (receipt.succeeded)
        {
            // A readback failure does not undo or relabel an accepted mutation.
            try
            {
                receipt.readbackSucceeded = queryStatus(request.config.serviceName,
                    &receipt.status, &receipt.readbackErrorText, &receipt.readbackErrorCode);
            }
            catch (const std::exception& error)
            {
                receipt.readbackErrorText = error.what();
            }
            catch (...)
            {
                receipt.readbackErrorText = "unknown exception";
            }
        }
        return receipt;
    }

    struct StatusRequest
    {
        std::wstring serviceName;
        std::uint64_t formRevision = 0;
    };

    struct StatusReceipt
    {
        StatusRequest request;
        bool succeeded = false;
        ks::service::ServiceStatus status{};
        std::string errorText;
        std::uint32_t errorCode = 0;
    };
}

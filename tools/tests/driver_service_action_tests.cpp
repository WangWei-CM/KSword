#include "../../Ksword5.1/Ksword5.1/DriverDock/DriverDock.ServiceAction.h"
#include <iostream>
#include <stdexcept>

using namespace ksword::driver_service_action;
static int checks = 0;
void require(bool condition, const char* text)
{
    ++checks;
    if (!condition) { throw std::runtime_error(text); }
}

int main()
{
    RegistrationRequest submitted;
    submitted.config.serviceName = L"MockDriver-A";
    submitted.config.displayName = L"Display A";
    submitted.config.binaryPath = L"\"X:\\Mock Driver.sys\"";
    submitted.config.description = L"Original description";
    submitted.config.startType = 2;
    submitted.config.errorControl = 3;
    submitted.unquotedPath = L"X:\\Mock Driver.sys";
    submitted.formRevision = 7;
    RegistrationRequest edited = submitted;
    int pathCalls = 0, mutationCalls = 0, queryCalls = 0;
    auto mutation = [&](const ks::service::KernelDriverServiceConfig& config,
        bool* created, std::string*, std::uint32_t*)
    {
        ++mutationCalls;
        require(config.serviceName == L"MockDriver-A", "service target changed");
        require(config.displayName == L"Display A", "display name omitted");
        require(config.binaryPath == submitted.config.binaryPath, "quoted path omitted");
        require(config.description == L"Original description", "description omitted");
        require(config.startType == 2 && config.errorControl == 3, "numeric config changed");
        *created = true;
        return true;
    };
    auto query = [&](const std::wstring& name, ks::service::ServiceStatus* status,
        std::string*, std::uint32_t*)
    {
        ++queryCalls;
        require(name == L"MockDriver-A", "readback queried a later form target");
        status->currentState = 4;
        return true;
    };
    const auto snapshot = submitted;
    // Mutate only a separate form value before the accepted worker starts.
    edited.config.serviceName = L"MockDriver-B";
    edited.config.description = L"Later edit";
    ++edited.formRevision;
    auto receipt = RunRegistration(snapshot, [&](const std::wstring& path)
    {
        ++pathCalls;
        require(path == submitted.unquotedPath, "path probe used later form path");
        return false;
    }, mutation, query);
    require(receipt.succeeded && receipt.created, "successful create was lost");
    require(receipt.pathUnavailable, "inaccessible path warning was lost");
    require(receipt.readbackSucceeded && receipt.status.currentState == 4, "readback missing");
    require(receipt.request.formRevision == 7 && edited.formRevision == 8, "form identity changed");
    require(mutationCalls == 1 && queryCalls == 1 && pathCalls == 1, "duplicate adapter call");

    auto special = submitted;
    special.systemRootPath = true;
    receipt = RunRegistration(special, [&](const std::wstring&) { ++pathCalls; return false; }, mutation, query);
    require(pathCalls == 1 && !receipt.pathUnavailable, "SystemRoot path began disk probe");

    const int previousQueries = queryCalls;
    receipt = RunRegistration(submitted, [](const std::wstring&) { return true; },
        [](const ks::service::KernelDriverServiceConfig&, bool*, std::string* error, std::uint32_t* code)
        { *error = "mock access denied"; *code = 5; return false; }, query);
    require(!receipt.succeeded && receipt.errorCode == 5 && receipt.errorText == "mock access denied",
        "mutation failure lost original error");
    require(queryCalls == previousQueries, "failed write started success readback");

    receipt = RunRegistration(submitted, [](const std::wstring&) { return true; }, mutation,
        [](const std::wstring&, ks::service::ServiceStatus*, std::string* error, std::uint32_t* code)
        { *error = "mock read denied"; *code = 13; return false; });
    require(receipt.succeeded && !receipt.readbackSucceeded && receipt.readbackErrorCode == 13,
        "readback failure incorrectly relabeled successful write");

    receipt = RunRegistration(submitted, [](const std::wstring&) { return true; }, mutation,
        [](const std::wstring&, ks::service::ServiceStatus*, std::string*, std::uint32_t*) -> bool
        { throw std::runtime_error("mock read exception"); });
    require(receipt.succeeded && !receipt.readbackSucceeded && receipt.readbackErrorText == "mock read exception",
        "readback exception hid completed mutation");
    std::cout << "PASS " << checks << " checks\n";
}

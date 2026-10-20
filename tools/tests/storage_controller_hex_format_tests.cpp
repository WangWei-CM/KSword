#include "../../Ksword5.1/Ksword5.1/MiscDock/DiskEditor/StorageControllerInput.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
int main()
{
    using namespace ks::misc::detail;
    unsigned checks = 0;
    const auto check = [&checks](bool value) { ++checks; if (!value) throw std::runtime_error("HEX full-document invariant"); };
    for (const std::size_t length : {1U, 31U, 32U, 33U, 4096U, 256U * 1024U})
    {
        std::vector<std::uint8_t> source(length);
        for (std::size_t index = 0; index < source.size(); ++index) source[index] = static_cast<std::uint8_t>((index * 73U + 19U) & 255U);
        const auto text = formatControllerHex(source);
        check(text.size() == length * 3U - 1U);
        check(text.back() != L'\n');
        check(std::count(text.begin(), text.end(), L'\n') == static_cast<std::ptrdiff_t>((length - 1U) / 32U));
        std::size_t lineLength = 0;
        for (const auto value : text)
        {
            if (value == L'\n') { check(lineLength == 95U); lineLength = 0; }
            else ++lineLength;
        }
        check(lineLength <= 95U);
        std::vector<std::uint8_t> parsed;
        check(parseControllerHex(text, static_cast<std::uint32_t>(length), parsed) && parsed == source);
        auto invalid = text;
        invalid[invalid.size()/2U] = L'Z';
        check(!parseControllerHex(invalid, static_cast<std::uint32_t>(length), parsed) && parsed.empty());
    }
    check(formatControllerHex({}).empty());
    std::cout << "CONTROLLER_HEX_FORMAT_CHECKS=" << checks << " FAILURES=0 GUI=0 BUSINESS_IO=0\n";
}

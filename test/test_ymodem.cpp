/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Zubax Robotics
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

// We want to ensure that assertion checks are enabled when tests are run, for extra safety
#ifdef NDEBUG
# undef NDEBUG
#endif

#define KOCHERGA_TRACE std::printf

// The library headers must be included first to make sure that they don't have any hidden include dependencies.
#include <kocherga_ymodem.hpp>

#include "catch.hpp"
#include "mocks.hpp"
#include "images.hpp"
#include "piped_process.hpp"

#include <thread>
#include <numeric>
#include <functional>
#include <iostream>
#include <utility>
#include <poll.h>


namespace
{

const char* const ValidImageFileName   = "valid-image.tmp";
const char* const InvalidImageFileName = "invalid-image.tmp";

/**
 * Writes the test images into files, for use with the sender process.
 */
void initImageFiles()
{
    if (std::ofstream f(ValidImageFileName, std::ios::binary | std::ios::out); f)
    {
        f.write(reinterpret_cast<const char*>(images::AppValid.data()),
                images::AppValid.size());
    }
    else
    {
        throw std::runtime_error("Image file init failure");
    }

    if (std::ofstream f(InvalidImageFileName, std::ios::binary | std::ios::out); f)
    {
        f.write(reinterpret_cast<const char*>(images::AppWithInvalidDescriptor.data()),
                images::AppWithInvalidDescriptor.size());
    }
    else
    {
        throw std::runtime_error("Image file init failure");
    }
}

/**
 * A serial port implementation that connects to the sender process via pipes.
 * Pipes are used in place of a proper serial port here.
 */
class QuasiSerialPort final : public kocherga_ymodem::IYModemSerialPort
{
    piped_process::PipedProcessPtr proc_;

public:
    explicit QuasiSerialPort(piped_process::PipedProcessPtr process) :
        proc_(std::move(process))
    {
        proc_->makeIONonBlocking();
    }

    Result emit(std::uint8_t byte, std::chrono::microseconds timeout) final
    {
        {
            ::pollfd pfd{};
            pfd.fd = proc_->getInputFD();
            pfd.events = POLLOUT;

            if (::poll(&pfd, 1, std::max(1, int(timeout.count() / 1000))) < 0)
            {
                return Result::Error;
            }

            if ((unsigned(pfd.revents) & unsigned(POLLOUT)) == 0)
            {
                return Result::Timeout;
            }
        }

        const auto out = proc_->writeInput(&byte, 1);
        if (out && *out == 1)
        {
            return Result::Success;
        }
        else if (out && *out < 1)
        {
            return Result::Timeout;
        }
        else
        {
            return Result::Error;
        }
    }

    Result receive(std::uint8_t& out_byte, std::chrono::microseconds timeout) final
    {
        {
            ::pollfd pfd{};
            pfd.fd = proc_->getOutputFD();
            pfd.events = POLLIN;

            if (::poll(&pfd, 1, std::max(1, int(timeout.count() / 1000))) < 0)
            {
                return Result::Error;
            }

            if ((unsigned(pfd.revents) & unsigned(POLLIN)) == 0)
            {
                return Result::Timeout;
            }
        }

        const auto out = proc_->readOutput(&out_byte, 1);
        if (out && *out == 1)
        {
            return Result::Success;
        }
        else if (out && *out < 1)
        {
            return Result::Timeout;
        }
        else
        {
            return Result::Error;
        }
    }
};

/// Standard control characters
struct ControlCharacters
{
    static constexpr std::uint8_t SOH = 0x01;
    static constexpr std::uint8_t STX = 0x02;
    static constexpr std::uint8_t EOT = 0x04;
    static constexpr std::uint8_t ACK = 0x06;
    static constexpr std::uint8_t NAK = 0x15;
    static constexpr std::uint8_t CAN = 0x18;
    static constexpr std::uint8_t C   = 0x43;
};

}


TEST_CASE("YModem-PortTest")
{
    QuasiSerialPort port(piped_process::launch(std::string("sz -vv --xmodem ") + ValidImageFileName));

    {
        std::uint8_t b{};
        const auto res = port.receive(b, std::chrono::microseconds(1000));
        std::cout << "Port read result: " << int(res) << std::endl;
        REQUIRE(QuasiSerialPort::Result::Timeout == res);
    }

    REQUIRE(QuasiSerialPort::Result::Success == port.emit(ControlCharacters::NAK, std::chrono::microseconds(1000)));

    static const auto get = [&]() -> std::uint8_t
    {
        std::uint8_t b{};
        const auto res = port.receive(b, std::chrono::microseconds(1000));
        REQUIRE(QuasiSerialPort::Result::Success == res);
        return b;
    };

    // Header
    REQUIRE(ControlCharacters::SOH == get());

    // Block number
    REQUIRE(1    == get());
    REQUIRE(0xFE == get());

    // Data - see the test image for reference
    for (std::uint8_t i = 0; i < 128; i++)
    {
        const auto b = get();
        REQUIRE(b == images::AppValid.at(i));
    }

    // Checksum
    {
        auto checksum = std::uint8_t(std::accumulate(images::AppValid.begin(),
                                                     images::AppValid.begin() + 128,
                                                     0U) & 0xFF);
        REQUIRE(checksum == get());
    }
}


TEST_CASE("YModem-Basic")
{
    static constexpr std::uint32_t ROMSize = 1024 * 1024;

    initImageFiles();

    mocks::Platform platform;
    mocks::FileMappedROMBackend rom_backend("ymodem-rom.tmp", ROMSize);

    kocherga::BootloaderController blc(platform, rom_backend, ROMSize, std::chrono::seconds(1));

//    QuasiSerialPort port(piped_process::launch("sz -vv --ymodem --1k " + ValidImageFileName));
//    kocherga_ymodem::YModemProtocol ym(platform, port);
//
//    (void) ym;
}

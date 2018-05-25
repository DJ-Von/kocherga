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

#pragma once

#include <kocherga.hpp>

// Third-party dependencies:
#include <popcop.hpp>                   // Popcop protocol implementation in C++
#include <senoval/string.hpp>           // Utility library for embedded systems
#include <senoval/vector.hpp>           // Utility library for embedded systems
#include <utility>


namespace kocherga_popcop
{
/**
 * Platform abstraction interface for the Popcop protocol.
 */
class IPopcopPlatform
{
public:
    /**
     * Serial port input/output methods should return if the IO operation could not be completed in this amount of time.
     */
    static constexpr std::chrono::microseconds IOByteTimeout{1'000};   // NOLINT

    /**
     * This constant is implicitly defined by Popcop. The protocol does not support CoA longer than this.
     */
    static constexpr std::uint8_t CertificateOfAuthenticityMaxLength = 255;

    virtual ~IPopcopPlatform() = default;

    /**
     * Sends one byte to the opposite endpoint.
     * If timed out, do nothing. @ref IOByteTimeout
     */
    virtual void emit(std::uint8_t byte) = 0;

    /**
     * Receives one byte from the serial port input buffer.
     * If timed out, returns an empty option. @ref IOByteTimeout
     */
    virtual std::optional<std::uint8_t> receive() = 0;

    /**
     * This method is invoked when the endpoint encounters a frame that it doesn't know how to process.
     * The application may opt to handle such frames itself.
     * The default implementation does nothing.
     */
    virtual void processUnhandledFrame(const popcop::transport::ParserOutput::Frame& frame)
    {
        (void) frame;
    }

    /**
     * This method is invoked when the local endpoint encounters unparsed data in the stream.
     * The application may opt to handle it in some way or print it.
     * The default implementation does nothing.
     */
    virtual void processExtraneousData(const std::uint8_t* data, std::size_t length)
    {
        (void) data;
        (void) length;
    }

    /**
     * This method is invoked when the local endpoint receives a device management command that it can't handle.
     * Currently, only the following device management commands are handled by the endpoint, all others are delegated
     * to the application via this method:
     *  - LaunchBootloader (does nothing)
     */
    virtual popcop::standard::DeviceManagementCommandResponseMessage::Status processUnhandledDeviceManagementCommand(
        const popcop::standard::DeviceManagementCommandRequestMessage& request) = 0;

    /**
     * This method, if implemented, must atomically write the certificate of authenticity into some kind of ROM,
     * and then read it back. The supplied data to write is pointed to by in_data, and the output data is pointed to
     * by out_data.
     * The return value is the length of out_data or a negative error code.
     * The length of CoA can never exceed 255 bytes.
     * If the target platform does not support CoA storage, leave this method unimplemented.
     */
    virtual std::int16_t writeAndReadBackCertificateOfAuthenticity(const std::uint8_t* in_data,
                                                                   std::uint8_t* out_data,
                                                                   std::uint8_t length)
    {
        (void) in_data;
        (void) out_data;
        (void) length;
        return 0;
    }

    /**
     * This method is invoked by the endpoint's thread periodically as long as it functions properly.
     * The application can use it to reset a watchdog, but it is not mandatory.
     * The minimal watchdog timeout is 3 seconds! Lower values may trigger spurious resets.
     */
    virtual void resetWatchdog() = 0;

    /**
     * This method is invoked by the endpoint periodically to check if it should terminate.
     */
    virtual bool shouldExit() const = 0;
};

/**
 * Popcop bootloader endpoint implementation.
 * Either instantiate one instance per available port, or switch the same instance between available ports.
 */
class PopcopProtocol final : private kocherga::IProtocol
{
    ::kocherga::BootloaderController& blc_;
    IPopcopPlatform& platform_;
    const popcop::standard::NodeInfoMessage node_info_prototype_;

    popcop::transport::Parser<> parser_{};

    kocherga::IDownloadSink* download_sink_ = nullptr;
    bool download_image_reached_ = false;
    std::int16_t upgrade_status_code_ = 0;


    // Sends out one frame, ignores errors
    template <typename M>
    void send(const M& message)
    {
        struct
        {
            IPopcopPlatform* pl_;
            void operator()(std::uint8_t byte)
            {
                pl_->emit(byte);
            }
        }
        sender
        {
            &platform_
        };
        (void) message.encode(popcop::transport::StreamEmitter(popcop::presentation::StandardFrameTypeCode,
                                                               sender).begin());
    }

    void processNodeInfoRequest()
    {
        popcop::standard::NodeInfoMessage m = node_info_prototype_;
        if (const auto ai = blc_.getAppInfo())
        {
            auto& sw = m.software_version;
            sw.major         = ai->major_version;
            sw.minor         = ai->minor_version;
            sw.vcs_commit_id = ai->vcs_commit;
            sw.image_crc     = ai->image_crc;
            sw.release_build = ai->isReleaseBuild();
            sw.dirty_build   = ai->isDirtyBuild();
            sw.build_timestamp_utc = ai->isBuildTimestampValid() ? ai->build_timestamp_utc : 0;
        }

        send(m);
    }

    void processDeviceManagementCommandRequest(const popcop::standard::DeviceManagementCommandRequestMessage& req)
    {
        popcop::standard::DeviceManagementCommandResponseMessage resp{};
        resp.command = req.command;

        if (req.command == popcop::standard::DeviceManagementCommand::LaunchBootloader)
        {
            // Do nothing - we're already in the bootloader!
            resp.status = popcop::standard::DeviceManagementCommandResponseMessage::Status::Ok;
        }
        else
        {
            resp.status = platform_.processUnhandledDeviceManagementCommand(req);
        }

        send(resp);
    }

    void sendBootloaderStatusResponse()
    {
        popcop::standard::BootloaderStatusResponseMessage resp{};
        resp.timestamp = blc_.getMonotonicUptime();

        switch (blc_.getState())
        {
        case kocherga::State::NoAppToBoot:
        {
            assert(download_sink_ == nullptr);
            resp.state = popcop::standard::BootloaderState::NoAppToBoot;
            break;
        }
        case kocherga::State::BootDelay:
        {
            assert(download_sink_ == nullptr);
            resp.state = popcop::standard::BootloaderState::BootDelay;
            break;
        }
        case kocherga::State::BootCancelled:
        {
            assert(download_sink_ == nullptr);
            resp.state = popcop::standard::BootloaderState::BootCancelled;
            break;
        }
        case kocherga::State::AppUpgradeInProgress:
        {
            assert(download_sink_ != nullptr);
            resp.state = popcop::standard::BootloaderState::AppUpgradeInProgress;
            break;
        }
        case kocherga::State::ReadyToBoot:
        {
            assert(download_sink_ == nullptr);
            resp.state = popcop::standard::BootloaderState::ReadyToBoot;
            break;
        }
        }

        send(resp);
    }

    void processBootloaderStatusRequest(const popcop::standard::BootloaderStatusRequestMessage& req)
    {
        switch (req.desired_state)
        {
        case popcop::standard::BootloaderState::BootCancelled:
        {
            blc_.cancelBoot();
            sendBootloaderStatusResponse();
            break;
        }

        case popcop::standard::BootloaderState::AppUpgradeInProgress:
        {
            if (download_sink_ == nullptr)
            {
                download_image_reached_ = false;
                upgrade_status_code_ = 0;

                // This function blocks for a long time; it will send the response itself
                (void) blc_.upgradeApp(*this);

                if (!download_image_reached_)       // Response has not been sent, do it now
                {
                    sendBootloaderStatusResponse();
                }
            }
            else
            {
                sendBootloaderStatusResponse();     // We're already upgrading, do nothing
            }
            break;
        }

        case popcop::standard::BootloaderState::ReadyToBoot:
        {
            blc_.requestBoot();
            sendBootloaderStatusResponse();
            break;
        }

        case popcop::standard::BootloaderState::NoAppToBoot:
        case popcop::standard::BootloaderState::BootDelay:
        {
            break;
        }
        }
    }

    void processBootloaderImageDataRequest(const popcop::standard::BootloaderImageDataRequestMessage& req)
    {
        popcop::standard::BootloaderImageDataResponseMessage resp{};
        resp.image_offset = req.image_offset;
        resp.image_type   = req.image_type;

        switch (req.image_type)
        {
        case popcop::standard::BootloaderImageType::Application:
        {
            if (download_sink_ != nullptr)
            {
                // Observe that we ignore the offset here! The protocol requires that the offset must grow sequentially.
                // If it doesn't, the downloaded image will be invalid; the bootloader controller will catch that later.
                if (!req.image_data.empty())
                {
                    const auto result = download_sink_->handleNextDataChunk(req.image_data.data(),
                                                                            std::uint16_t(req.image_data.size()));
                    if (result >= 0)
                    {
                        resp.image_data = req.image_data;
                        upgrade_status_code_ = 0;
                    }
                    else
                    {
                        upgrade_status_code_ = result;
                    }
                }

                if (req.image_data.size() < req.image_data.max_size())
                {
                    // Last chunk received, terminate
                    download_sink_ = nullptr;
                }
            }
        }

        case popcop::standard::BootloaderImageType::CertificateOfAuthenticity:
        {
            if ((req.image_offset == 0) &&
                (req.image_data.size() < IPopcopPlatform::CertificateOfAuthenticityMaxLength))
            {
                resp.image_data.resize(resp.image_data.max_size());
                const auto result =
                    platform_.writeAndReadBackCertificateOfAuthenticity(req.image_data.data(),
                                                                        resp.image_data.data(),
                                                                        std::uint8_t(req.image_data.size()));
                if (result >= 0)
                {
                    resp.image_data.resize(std::size_t(result));
                }
                else
                {
                    resp.image_data.clear();
                }
            }
            else
            {
                ;   // Invalid request, return empty
            }
            break;
        }
        }

        send(resp);
    }

    void processFrame(const popcop::transport::ParserOutput::Frame& frame)
    {
        if (frame.type_code == popcop::presentation::StandardFrameTypeCode)
        {
            const auto& payload = frame.payload;

            if (popcop::standard::NodeInfoMessage::tryDecode(payload.begin(),
                                                             payload.end()))
            {
                processNodeInfoRequest();
            }
            else if (auto dmcr = popcop::standard::DeviceManagementCommandRequestMessage::tryDecode(payload.begin(),
                                                                                                    payload.end()))
            {
                processDeviceManagementCommandRequest(*dmcr);
            }
            else if (auto bsr = popcop::standard::BootloaderStatusRequestMessage::tryDecode(payload.begin(),
                                                                                            payload.end()))
            {
                processBootloaderStatusRequest(*bsr);
            }
            else if (auto bidr = popcop::standard::BootloaderImageDataRequestMessage::tryDecode(payload.begin(),
                                                                                                payload.end()))
            {
                processBootloaderImageDataRequest(*bidr);
            }
            else
            {
                platform_.processUnhandledFrame(frame);
            }
        }
        else
        {
            platform_.processUnhandledFrame(frame);
        }
    }

    void processByte(std::uint8_t byte)
    {
        const auto out = parser_.processNextByte(byte);
        if (auto frame = out.getReceivedFrame())
        {
            platform_.resetWatchdog();
            processFrame(*frame);
            platform_.resetWatchdog();
        }
        else if (auto ed = out.getExtraneousData())
        {
            platform_.resetWatchdog();
            platform_.processExtraneousData(ed->data(), ed->size());
            platform_.resetWatchdog();
        }
        else
        {
            ;
        }
    }

    void loopOnce()
    {
        platform_.resetWatchdog();
        if (const auto res = platform_.receive())
        {
            processByte(*res);
        }
    }

    std::int16_t downloadImage(kocherga::IDownloadSink& sink) final
    {
        assert(!download_image_reached_);
        assert(download_sink_ == nullptr);
        assert(upgrade_status_code_ == 0);

        download_sink_ = &sink;

        download_image_reached_ = true;
        sendBootloaderStatusResponse();

        while (!platform_.shouldExit() &&
               (download_sink_ != nullptr) &&
               (upgrade_status_code_ >= 0))
        {
            loopOnce();
        }

        download_sink_ = nullptr;
        return upgrade_status_code_;
    }

    static popcop::standard::NodeInfoMessage prepareNodeInfoMessage(popcop::standard::NodeInfoMessage prototype)
    {
        prototype.software_version = popcop::standard::NodeInfoMessage::SoftwareVersion();
        prototype.mode = popcop::standard::NodeInfoMessage::Mode::Bootloader;
        return prototype;
    }

public:
    PopcopProtocol(::kocherga::BootloaderController& bootloader_controller,
                   IPopcopPlatform& popcop_platform,
                   const popcop::standard::NodeInfoMessage& ni) :
        blc_(bootloader_controller),
        platform_(popcop_platform),
        node_info_prototype_(prepareNodeInfoMessage(ni))
    { }

    /**
     * Runs the endpoint thread.
     * This function never returns unless IPopcopPlatform::shouldExit() returns true.
     * If an RTOS is available, it is advisable to run this method from a separate thread.
     * Otherwise, it is possible to perform other tasks by hijacking certain platform API functions.
     */
    void run()
    {
        while (!platform_.shouldExit())
        {
            loopOnce();
        }
    }
};

}  // namespace kocherga_popcop

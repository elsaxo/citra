// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <catch.hpp>
#include "core/hle/ipc.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/server_session.h"

namespace Kernel {

static SharedPtr<Object> MakeObject() {
    return Event::Create(ResetType::OneShot);
}

TEST_CASE("HLERequestContext::PopulateFromIncomingCommandBuffer", "[core][kernel]") {
    auto session = std::get<SharedPtr<ServerSession>>(ServerSession::CreateSessionPair());
    HLERequestContext context(std::move(session));

    auto process = Process::Create(CodeSet::Create("", 0));
    HandleTable handle_table;

    SECTION("works with empty cmdbuf") {
        const u32_le input[]{
            IPC::MakeHeader(0x1234, 0, 0),
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        REQUIRE(context.CommandBuffer()[0] == 0x12340000);
    }

    SECTION("translates regular params") {
        const u32_le input[]{
            IPC::MakeHeader(0, 3, 0), 0x12345678, 0x21122112, 0xAABBCCDD,
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        auto* output = context.CommandBuffer();
        REQUIRE(output[1] == 0x12345678);
        REQUIRE(output[2] == 0x21122112);
        REQUIRE(output[3] == 0xAABBCCDD);
    }

    SECTION("translates move handles") {
        auto a = MakeObject();
        Handle a_handle = handle_table.Create(a).Unwrap();
        const u32_le input[]{
            IPC::MakeHeader(0, 0, 2), IPC::MoveHandleDesc(1), a_handle,
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        auto* output = context.CommandBuffer();
        REQUIRE(context.GetIncomingHandle(output[2]) == a);
        REQUIRE(handle_table.GetGeneric(a_handle) == nullptr);
    }

    SECTION("translates copy handles") {
        auto a = MakeObject();
        Handle a_handle = handle_table.Create(a).Unwrap();
        const u32_le input[]{
            IPC::MakeHeader(0, 0, 2), IPC::CopyHandleDesc(1), a_handle,
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        auto* output = context.CommandBuffer();
        REQUIRE(context.GetIncomingHandle(output[2]) == a);
        REQUIRE(handle_table.GetGeneric(a_handle) == a);
    }

    SECTION("translates multi-handle descriptors") {
        auto a = MakeObject();
        auto b = MakeObject();
        auto c = MakeObject();
        const u32_le input[]{
            IPC::MakeHeader(0, 0, 5),        IPC::MoveHandleDesc(2),
            handle_table.Create(a).Unwrap(), handle_table.Create(b).Unwrap(),
            IPC::MoveHandleDesc(1),          handle_table.Create(c).Unwrap(),
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        auto* output = context.CommandBuffer();
        REQUIRE(context.GetIncomingHandle(output[2]) == a);
        REQUIRE(context.GetIncomingHandle(output[3]) == b);
        REQUIRE(context.GetIncomingHandle(output[5]) == c);
    }

    SECTION("translates null handles") {
        const u32_le input[]{
            IPC::MakeHeader(0, 0, 2), IPC::MoveHandleDesc(1), 0,
        };

        auto result = context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        REQUIRE(result == RESULT_SUCCESS);
        auto* output = context.CommandBuffer();
        REQUIRE(context.GetIncomingHandle(output[2]) == nullptr);
    }

    SECTION("translates CallingPid descriptors") {
        const u32_le input[]{
            IPC::MakeHeader(0, 0, 2), IPC::CallingPidDesc(), 0x98989898,
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        REQUIRE(context.CommandBuffer()[2] == process->process_id);
    }

    SECTION("translates StaticBuffer descriptors") {
        auto buffer = std::make_shared<std::vector<u8>>(Memory::PAGE_SIZE);
        std::fill(buffer->begin(), buffer->end(), 0xAB);

        VAddr target_address = 0x10000000;
        auto result = process->vm_manager.MapMemoryBlock(target_address, buffer, 0, buffer->size(),
                                                         MemoryState::Private);
        REQUIRE(result.Code() == RESULT_SUCCESS);

        const u32_le input[]{
            IPC::MakeHeader(0, 0, 2), IPC::StaticBufferDesc(buffer->size(), 0), target_address,
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        CHECK(context.GetStaticBuffer(0) == *buffer);

        REQUIRE(process->vm_manager.UnmapRange(target_address, buffer->size()) == RESULT_SUCCESS);
    }

    SECTION("translates mixed params") {
        auto buffer = std::make_shared<std::vector<u8>>(Memory::PAGE_SIZE);
        std::fill(buffer->begin(), buffer->end(), 0xCE);

        VAddr target_address = 0x10000000;
        auto result = process->vm_manager.MapMemoryBlock(target_address, buffer, 0, buffer->size(),
                                                         MemoryState::Private);
        REQUIRE(result.Code() == RESULT_SUCCESS);

        auto a = MakeObject();
        const u32_le input[]{
            IPC::MakeHeader(0, 2, 6),
            0x12345678,
            0xABCDEF00,
            IPC::MoveHandleDesc(1),
            handle_table.Create(a).Unwrap(),
            IPC::CallingPidDesc(),
            0,
            IPC::StaticBufferDesc(buffer->size(), 0),
            target_address,
        };

        context.PopulateFromIncomingCommandBuffer(input, *process, handle_table);

        auto* output = context.CommandBuffer();
        CHECK(output[1] == 0x12345678);
        CHECK(output[2] == 0xABCDEF00);
        CHECK(context.GetIncomingHandle(output[4]) == a);
        CHECK(output[6] == process->process_id);
        CHECK(context.GetStaticBuffer(0) == *buffer);

        REQUIRE(process->vm_manager.UnmapRange(target_address, buffer->size()) == RESULT_SUCCESS);
    }
}

TEST_CASE("HLERequestContext::WriteToOutgoingCommandBuffer", "[core][kernel]") {
    auto session = std::get<SharedPtr<ServerSession>>(ServerSession::CreateSessionPair());
    HLERequestContext context(std::move(session));

    auto process = Process::Create(CodeSet::Create("", 0));
    HandleTable handle_table;
    auto* input = context.CommandBuffer();
    u32_le output[IPC::COMMAND_BUFFER_LENGTH];

    SECTION("works with empty cmdbuf") {
        input[0] = IPC::MakeHeader(0x1234, 0, 0);

        context.WriteToOutgoingCommandBuffer(output, *process, handle_table);

        REQUIRE(output[0] == 0x12340000);
    }

    SECTION("translates regular params") {
        input[0] = IPC::MakeHeader(0, 3, 0);
        input[1] = 0x12345678;
        input[2] = 0x21122112;
        input[3] = 0xAABBCCDD;

        context.WriteToOutgoingCommandBuffer(output, *process, handle_table);

        REQUIRE(output[1] == 0x12345678);
        REQUIRE(output[2] == 0x21122112);
        REQUIRE(output[3] == 0xAABBCCDD);
    }

    SECTION("translates move/copy handles") {
        auto a = MakeObject();
        auto b = MakeObject();
        input[0] = IPC::MakeHeader(0, 0, 4);
        input[1] = IPC::MoveHandleDesc(1);
        input[2] = context.AddOutgoingHandle(a);
        input[3] = IPC::CopyHandleDesc(1);
        input[4] = context.AddOutgoingHandle(b);

        context.WriteToOutgoingCommandBuffer(output, *process, handle_table);

        REQUIRE(handle_table.GetGeneric(output[2]) == a);
        REQUIRE(handle_table.GetGeneric(output[4]) == b);
    }

    SECTION("translates null handles") {
        input[0] = IPC::MakeHeader(0, 0, 2);
        input[1] = IPC::MoveHandleDesc(1);
        input[2] = context.AddOutgoingHandle(nullptr);

        auto result = context.WriteToOutgoingCommandBuffer(output, *process, handle_table);

        REQUIRE(result == RESULT_SUCCESS);
        REQUIRE(output[2] == 0);
    }

    SECTION("translates multi-handle descriptors") {
        auto a = MakeObject();
        auto b = MakeObject();
        auto c = MakeObject();
        input[0] = IPC::MakeHeader(0, 0, 5);
        input[1] = IPC::MoveHandleDesc(2);
        input[2] = context.AddOutgoingHandle(a);
        input[3] = context.AddOutgoingHandle(b);
        input[4] = IPC::CopyHandleDesc(1);
        input[5] = context.AddOutgoingHandle(c);

        context.WriteToOutgoingCommandBuffer(output, *process, handle_table);

        REQUIRE(handle_table.GetGeneric(output[2]) == a);
        REQUIRE(handle_table.GetGeneric(output[3]) == b);
        REQUIRE(handle_table.GetGeneric(output[5]) == c);
    }

    SECTION("translates StaticBuffer descriptors") {
        std::vector<u8> input_buffer(Memory::PAGE_SIZE);
        std::fill(input_buffer.begin(), input_buffer.end(), 0xAB);

        context.AddStaticBuffer(0, input_buffer);

        auto output_buffer = std::make_shared<std::vector<u8>>(Memory::PAGE_SIZE);
        VAddr target_address = 0x10000000;
        auto result = process->vm_manager.MapMemoryBlock(
            target_address, output_buffer, 0, output_buffer->size(), MemoryState::Private);
        REQUIRE(result.Code() == RESULT_SUCCESS);

        input[0] = IPC::MakeHeader(0, 0, 2);
        input[1] = IPC::StaticBufferDesc(input_buffer.size(), 0);
        input[2] = target_address;

        // An entire command buffer plus enough space for one static buffer descriptor and its
        // target address
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH + 2> output_cmdbuff;
        // Set up the output StaticBuffer
        output_cmdbuff[IPC::COMMAND_BUFFER_LENGTH] =
            IPC::StaticBufferDesc(output_buffer->size(), 0);
        output_cmdbuff[IPC::COMMAND_BUFFER_LENGTH + 1] = target_address;

        context.WriteToOutgoingCommandBuffer(output_cmdbuff.data(), *process, handle_table);

        CHECK(*output_buffer == input_buffer);
        REQUIRE(process->vm_manager.UnmapRange(target_address, output_buffer->size()) ==
                RESULT_SUCCESS);
    }
}

} // namespace Kernel

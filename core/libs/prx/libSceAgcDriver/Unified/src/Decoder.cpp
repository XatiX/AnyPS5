#include "prx/libSceAgcDriver/Unified/include/Decoder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawRegisterKey.hpp"
#include <algorithm>
#include <optional>
#include <cstring>
#include <exception>
#include <span>

namespace AgcDriver::Unified {

namespace {

enum Bank : std::uint8_t {
    BankContext = 1,
    BankShader = 2,
    BankUserConfig = 4,
    BankScalars = 8,
    BankAll = 15,
};

std::uint64_t join(std::uint32_t low, std::uint32_t high) {
    return low | (static_cast<std::uint64_t>(high) << 32u);
}

std::uint8_t touchedBanks(std::span<const std::uint32_t> packet, std::uint32_t opcode) {
    switch (opcode) {
        case 0x10:
            switch ((packet[0] >> 2u) & 0x3fu) {
                case 0x09: return BankAll;
                case 0x1a: return BankContext;
                default: return 0;
            }
        case 0x11: case 0x13: case 0x26: case 0x2a: case 0x2f: return BankScalars;
        case 0x12: case 0x51: case 0x69: case 0x9f: return BankContext;
        case 0x63: case 0x76: return BankShader;
        case 0x64: case 0x79: case 0x7a: return BankUserConfig | BankScalars;
        case 0x37: return Pm4::CopyDataToRegister(packet) ? BankUserConfig | BankScalars : 0;
        default: return 0;
    }
}

bool sealsPassWrites(std::span<const std::uint32_t> packet) {
    if (packet.size() < 2) return false;
    switch (packet[1] & 0x3fu) {
        case 0x07: case 0x0f: case 0x10: case 0x16: case 0x2a: case 0x2c: case 0x2e: case 0x31: return true;
        default: return false;
    }
}

bool executesWithoutMemory(std::span<const std::uint32_t> packet, std::uint32_t opcode) {
    switch (opcode) {
        case 0x10: {
            const auto operation = (packet[0] >> 2u) & 0x3fu;
            return operation == 0 || operation == 0x09 || operation == 0x0b || operation == 0x0c || operation == 0x1a;
        }
        case 0x11: case 0x12: case 0x13: case 0x20: case 0x26: case 0x28: case 0x2a: case 0x2f: case 0x51: case 0x59:
        case 0x69: case 0x76: case 0x79: case 0x7a: case 0x81:
            return true;
        case 0x37: return packet.size() > 4 && Pm4::CopyDataToRegister(packet);
        case 0x46: return Pm4::SampleControl(packet);
        default: return false;
    }
}

bool changesStateFromMemory(std::span<const std::uint32_t> packet, std::uint32_t opcode) {
    switch (opcode) {
        case 0x45: case 0x91: return true;
        case 0x40: return packet.size() >= 6 && Pm4::CopyDataToRegister(packet);
        default: return false;
    }
}

DrawScalars scalarsOf(const QueueState& queue) {
    return {queue.indexBase, queue.drawIndirectBase, queue.dispatchIndirectBase, queue.indexBufferSize.value_or(0), queue.indexBufferSize.has_value(), queue.indexType, queue.instanceCount};
}

}

class Decoder {
public:
    Decoder(const DriverDetail::Submission& submission, DecodeState& state, Sink& sink) : submission(submission), state(state), sink(sink) {}

    void Run() {
        const auto& commands = submission.commands;
        for (std::size_t cursor = 0; cursor < commands.size();) {
            const auto header = commands[cursor];
            if (Pm4::FillerPacket(header)) {
                ++cursor;
                continue;
            }
            const auto count = Pm4::PacketWords(header);
            if (count > commands.size() - cursor) {
                ++state.counts.truncated;
                return;
            }
            decodePacket(std::span(commands).subspan(cursor, count), cursor);
            cursor += count;
        }
    }

private:
    void decodePacket(std::span<const std::uint32_t> packet, std::size_t cursor) {
        const auto header = packet[0];
        const auto opcode = (header >> 8u) & 0xffu;
        const bool predicatedPacket = Pm4::Predicated(header) && state.state.predication.operation != 0;
        const bool conditional = cursor < conditionalUntil || predicatedPacket;
        current = {cursor, opcode, conditional};
        if (!ranges.empty()) std::erase_if(ranges, [cursor](const OperationGuard& guard) { return guard.end <= cursor; });
        state.guards.assign(ranges.begin(), ranges.end());
        if (predicatedPacket) state.guards.push_back(predicationGuard(cursor, cursor + packet.size()));

        if (opcode == 0x3f || opcode == 0x22) {
            const auto end = submission.conditionalEnds.find(cursor);
            if (end != submission.conditionalEnds.end() && (opcode == 0x22 || conditional)) {
                conditionalUntil = std::max(conditionalUntil, end->second);
                if (opcode == 0x22 && packet.size() == 5u) {
                    OperationGuard guard{submission.serial, cursor, end->second, true};
                    std::copy(packet.begin(), packet.end(), guard.words.begin());
                    ranges.push_back(guard);
                } else if (opcode == 0x3f && predicatedPacket) {
                    ranges.push_back(predicationGuard(cursor, end->second));
                }
            }
            return;
        }
        if (header == RenderingWaitPacketHeader) {
            ++state.counts.waits;
            emit(OpKind::Wait, 0, 0, 0);
            return;
        }
        if (submission.flips.contains(cursor)) {
            if (opcode == 0x49) labelStore(packet);
            ++state.counts.flips;
            emit(OpKind::Flip, 0, 0, submission.flipHandles.contains(cursor) ? submission.flipHandles.at(cursor) : 0);
            return;
        }
        if (Pm4::DrawOpcode(opcode)) {
            draw(Pm4::IndirectDrawOpcode(opcode) ? OpKind::DrawIndirect : OpKind::Draw, packet);
            return;
        }
        switch (opcode) {
            case 0x15: case 0x16: dispatch(packet, opcode); return;
            case 0x3c: case 0x93: wait(packet); return;
            case 0x39:
                if (packet.size() < 3) return;
                ++state.counts.waits;
                emit(OpKind::Wait, join(packet[1], packet[2]), 8, 0);
                return;
            case 0x49: labelStore(packet); return;
            case 0x37:
                if (!Pm4::CopyDataToRegister(packet)) {
                    labelStore(packet);
                    return;
                }
                break;
            case 0x40: case 0x50: case 0x83:
                if (!changesStateFromMemory(packet, opcode)) {
                    store(packet, opcode);
                    return;
                }
                break;
            case 0x63: case 0x64: case 0x9f: indirectRegisters(packet, opcode); return;
            case 0x46:
                if (Pm4::SampleDump(packet)) {
                    store(packet, opcode);
                    return;
                }
                if (sealsPassWrites(packet)) emit(OpKind::Marker, 0, 0, 0);
                break;
            case 0x58:
                emit(OpKind::Marker, 0, 0, 0);
                return;
            default: break;
        }
        if (changesStateFromMemory(packet, opcode)) {
            ++state.counts.stateFromMemory;
            return;
        }
        if (!executesWithoutMemory(packet, opcode)) return;
        try {
            Pm4::Execute(packet, state.state);
        } catch (const std::exception&) {
            ++state.counts.stateErrors;
        }
        markDirty(touchedBanks(packet, opcode));
    }

    OperationGuard predicationGuard(std::size_t start, std::size_t end) const {
        OperationGuard guard{submission.serial, start, end, false};
        guard.predication = state.state.predication;
        guard.sampleDumpBlocks = state.state.sampleDumpBlocks;
        return guard;
    }

    void markDirty(std::uint8_t banks) {
        state.contextDirty |= (banks & BankContext) != 0;
        state.shaderDirty |= (banks & BankShader) != 0;
        state.userConfigDirty |= (banks & BankUserConfig) != 0;
        state.scalarsDirty |= (banks & BankScalars) != 0;
    }

    template <typename TRing, typename TValue>
    std::uint64_t take(TRing& ring, const TValue& live, bool& dirty) {
        typename TRing::View view;
        while (!ring.TryTake(live, dirty, view)) {
            ++state.counts.snapshotStalls;
            sink.MakeRoom();
        }
        return view.id;
    }

    SnapshotIds snapshot() {
        SnapshotIds ids;
        ids.context = take(state.context, state.state.context, state.contextDirty);
        ids.userConfig = take(state.userConfig, state.state.userConfig, state.userConfigDirty);
        ids.shader = take(state.shader, state.state.shader, state.shaderDirty);
        ids.scalars = take(state.scalars, scalarsOf(state.state), state.scalarsDirty);
        return ids;
    }

    void emit(OpKind kind, std::uint64_t address, std::uint64_t size, std::uint64_t value, const SnapshotIds& snapshots = {}, std::uint64_t mask = ~std::uint64_t{0}, std::uint8_t function = 0) {
        Operation operation;
        operation.kind = kind;
        operation.opcode = static_cast<std::uint8_t>(current.opcode);
        operation.predicated = current.conditional;
        operation.queue = state.queueId;
        operation.sequence = ++state.sequence;
        operation.submission = submission.serial;
        operation.packetOffset = static_cast<std::uint32_t>(current.cursor);
        operation.snapshots = snapshots;
        operation.address = address;
        operation.size = size;
        operation.value = value;
        operation.mask = mask;
        operation.function = function;
        if (resolvedKey.has_value()) {
            operation.resolved = true;
            operation.key = *resolvedKey;
            operation.draw = resolvedDraw;
            resolvedKey.reset();
        }
        if (IsDispatch(kind)) operation.dispatch = pendingDispatch;
        ++state.counts.operations;
        if (current.conditional) ++state.counts.conditional;
        sink.Emit(operation);
    }

    void draw(OpKind kind, std::span<const std::uint32_t> packet) {
        ++state.counts.draws;
        if (current.conditional) ++state.counts.conditionalDraws;
        const auto ids = snapshot();
        const std::uint64_t address = kind == OpKind::DrawIndirect && packet.size() >= 2 ? state.state.drawIndirectBase + packet[1] : 0;
        if (state.resolve && resolveDraw(kind, packet, ids)) return;
        emit(kind, address, 0, packet.size() >= 2 ? packet[1] : 0, ids);
    }

    bool resolveDraw(OpKind kind, std::span<const std::uint32_t> packet, const SnapshotIds& ids) {
        Pm4::DrawParameters draw{};
        std::uint64_t key = 0;
        try {
            draw = Pm4::ResolveDraw(packet, state.state);
            key = DriverDetail::RegisterStateKey(state.state);
        } catch (const std::exception&) {
            ++state.counts.resolveErrors;
            return false;
        }
        ++state.counts.resolved;
        if (state.uses) reportDrawUses(draw, ids);
        const std::uint64_t address = draw.indirect.has_value() ? draw.indirect->arguments : draw.indexAddress;
        const std::uint64_t size = draw.indirect.has_value() ? draw.indirect->RangeBytes() : draw.indexCount;
        resolvedKey = key;
        resolvedDraw = {draw.indexAddress, draw.indexCount, draw.indexSize, draw.instanceCount, draw.flags, draw.firstVertex, draw.firstInstance, draw.indexed, draw.indirect.has_value()};
        if (draw.indirect) {
            resolvedDraw.indirectAddress = draw.indirect->arguments;
            resolvedDraw.indirectBytes = draw.indirect->RangeBytes();
            if (draw.indirect->countIndirect) resolvedDraw.countAddress = draw.indirect->countAddress;
        }
        emit(kind, address, size, draw.instanceCount | (static_cast<std::uint64_t>(draw.indexSize) << 32u), ids);
        return true;
    }

    void reportDrawUses(const Pm4::DrawParameters& draw, const SnapshotIds& ids) {
        auto& buffer = state.useBuffer;
        buffer.clear();
        DrawUses(draw, buffer);
        if (ids.context != state.targetContext) {
            state.targetContext = ids.context;
            try {
                TargetUses(state.state, buffer);
            } catch (const std::exception&) {
                ++state.counts.useErrors;
            }
        }
        if (buffer.empty()) return;
        ++state.counts.useReports;
        state.uses(buffer);
    }

    void dispatch(std::span<const std::uint32_t> packet, std::uint32_t opcode) {
        const bool direct = opcode == 0x15;
        if ((direct && packet.size() != 5) || (!direct && packet.size() != 3 && packet.size() != 4)) {
            ++state.counts.stateErrors;
            return;
        }
        ++state.counts.dispatches;
        if (current.conditional) ++state.counts.conditionalDispatches;
        std::uint64_t address = 0;
        if (!direct) {
            try {
                address = Pm4::DispatchArgumentAddress(packet, state.state);
            } catch (const std::exception&) {
                ++state.counts.stateErrors;
            }
        }
        const auto ids = snapshot();
        pendingDispatch = {};
        pendingDispatch.initiator = packet.back();
        pendingDispatch.indirect = !direct;
        pendingDispatch.indirectAddress = address;
        if (direct) {
            pendingDispatch.groupsX = packet[1];
            pendingDispatch.groupsY = packet[2];
            pendingDispatch.groupsZ = packet[3];
        }
        if (direct) emit(OpKind::Dispatch, 0, packet[3], join(packet[1], packet[2]), ids);
        else emit(OpKind::DispatchIndirect, address, 12, 0, ids);
        if (state.uses && !direct && address != 0) {
            const GuestUse use{address, 12, false};
            ++state.counts.useReports;
            state.uses(std::span(&use, 1));
        }
        noteDispatch(packet, direct);
    }

    void noteDispatch(std::span<const std::uint32_t> packet, bool direct) {
        auto& shader = state.state.shader;
        const auto holds = [&](std::uint32_t offset, std::uint32_t value) { return shader.contains(offset) && shader.at(offset) == value; };
        bool same = holds(0x200, packet.back());
        for (std::uint32_t i = 0; i < 3 && same; ++i) same = direct ? holds(0x201 + i, packet[1 + i]) : !shader.contains(0x201 + i);
        if (same) return;
        Pm4::NoteDispatchRegisters(packet, state.state);
        state.shaderDirty = true;
    }

    void wait(std::span<const std::uint32_t> packet) {
        try {
            const auto fields = Pm4::DecodeWait(packet);
            ++state.counts.waits;
            emit(OpKind::Wait, fields.address, fields.bytes, fields.reference, {}, fields.mask, static_cast<std::uint8_t>(fields.function));
        } catch (const std::exception&) {
            ++state.counts.stateErrors;
        }
    }

    void labelStore(std::span<const std::uint32_t> packet) {
        const auto label = Pm4::DecodeLabelWrite(packet);
        if (!label.has_value()) return;
        const auto bytes = label->Bytes();
        std::uint64_t value = 0;
        std::memcpy(&value, bytes.data(), std::min(bytes.size(), sizeof(value)));
        ++state.counts.labelStores;
        emit(OpKind::LabelStore, label->address, bytes.size(), value);
    }

    void store(std::span<const std::uint32_t> packet, std::uint32_t opcode) {
        std::vector<Pm4::PacketStore> stores;
        try {
            stores = Pm4::DecodeStores(packet);
        } catch (const std::exception&) {
            ++state.counts.stateErrors;
            return;
        }
        if (stores.empty()) return;
        const auto& first = stores.front();
        if (first.value == Pm4::PacketStore::Value::Pattern) {
            std::uint32_t value = 0;
            std::memcpy(&value, first.data.data(), std::min(first.data.size(), sizeof(value)));
            if (opcode == 0x50) {
                emit(OpKind::Fill, first.address, first.bytes, value);
            } else {
                ++state.counts.labelStores;
                emit(OpKind::LabelStore, first.address, first.bytes, value);
            }
            return;
        }
        std::uint64_t source = 0;
        if (const auto copy = Pm4::DecodeMemoryCopy(packet)) source = copy->source;
        else if (opcode == 0x40 && packet.size() >= 4) source = join(packet[2], packet[3]);
        emit(OpKind::Copy, first.address, first.bytes, source);
    }

    void indirectRegisters(std::span<const std::uint32_t> packet, std::uint32_t opcode) {
        const auto listed = submission.registerLists.find(current.cursor);
        if (listed == submission.registerLists.end() || !Pm4::IndirectRegistersValid(listed->second)) {
            ++state.counts.stateFromMemory;
            return;
        }
        try {
            Pm4::ExecuteIndirectRegisters(packet, listed->second, state.state);
        } catch (const std::exception&) {
            ++state.counts.stateErrors;
        }
        markDirty(touchedBanks(packet, opcode));
    }

    struct Current {
        std::size_t cursor = 0;
        std::uint32_t opcode = 0;
        bool conditional = false;
    };

    const DriverDetail::Submission& submission;
    DecodeState& state;
    Sink& sink;
    std::size_t conditionalUntil = 0;
    std::vector<OperationGuard> ranges;
    Current current;
    std::optional<std::uint64_t> resolvedKey;
    DispatchFacts pendingDispatch;
    DrawFacts resolvedDraw;
};

void DecodeState::RetireBefore(const SnapshotIds& ids) {
    if (ids.context > 1) context.Retire(ids.context - 1);
    if (ids.userConfig > 1) userConfig.Retire(ids.userConfig - 1);
    if (ids.shader > 1) shader.Retire(ids.shader - 1);
    if (ids.scalars > 1) scalars.Retire(ids.scalars - 1);
}

void DecodeSubmission(const DriverDetail::Submission& submission, DecodeState& state, Sink& sink) {
    state.submission = &submission;
    try {
        Decoder(submission, state, sink).Run();
    } catch (...) {
        state.submission = nullptr;
        throw;
    }
    state.submission = nullptr;
}

}

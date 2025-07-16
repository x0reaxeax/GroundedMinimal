#include "ItemSpawner.hpp"
#include "C2Cycle.hpp"
#include "Command.hpp"
#include <iostream>

namespace Command {
    std::mutex CommandBufferMutex = {};
    std::condition_variable CommandBufferCondition = {};
    std::atomic<bool> CommandBufferCookedForExecution{false}; // Atomic variable

    CommandBuffer GameCommandBuffer = {
        .Id = CommandId::CmdIdNone,
        .Params = nullptr
    };

    void ProcessCommands(void) {
        std::unique_lock<std::mutex> lockUnique(CommandBufferMutex);
        
        if (!CommandBufferCookedForExecution.load()) {
            return;
        }

        // Copy command data
        CommandBuffer localBuffer = GameCommandBuffer;
        
        // Reset the buffer immediately while holding lock
        GameCommandBuffer.Id = CommandId::CmdIdNone;
        GameCommandBuffer.Params = nullptr;
        GameCommandBuffer.Deleter = nullptr;

        // Process command without holding lock
        switch (localBuffer.Id) {
            case CommandId::CmdIdSpawnItem: {    
                LogMessage("ProcessEvent", "Command: Spawn Item");
                
                auto* lpParams = static_cast<ItemSpawner::BufferParamsSpawnItem*>(localBuffer.Params);
                bool bRet = ItemSpawner::GiveItemToPlayer(
                    lpParams->iPlayerId,
                    lpParams->szItemName,
                    lpParams->szDataTableName,
                    lpParams->iCount
                );

                LogMessage(
                    "ProcessEvent", 
                    "ItemSpawn - " + std::string(bRet ? "Success" : "Failure")
                );
                break;
            }

            case CommandId::CmdIdC2Cycle: {
                // Insert a newline to separate output from interpreter read symbol
                // this is normally not needed, because stuff is either entered from the interpreter directly,
                // or the output is usually disabled completely, but it's beneficial to log C2Cycle execution.
                LogChar(' ');
                LogMessage("ProcessEvent", "Command: C2 Cycle");

                C2Cycle::C2Cycle();
                break;
            }

            default: {
                LogError(
                    "ProcessEvent", 
                    "Unknown Command ID: " + std::to_string(static_cast<uint16_t>(localBuffer.Id))
                );
                break;
            }
        }
        
        // Clean up memory
        if (nullptr != localBuffer.Deleter && nullptr != localBuffer.Params) {
            localBuffer.Deleter(localBuffer.Params);
        }

        CommandBufferCookedForExecution.store(false);

        lockUnique.unlock();
        CommandBufferCondition.notify_all();
    }
}
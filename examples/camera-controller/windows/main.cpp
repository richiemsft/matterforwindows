/*
 * Copyright (c) 2026 Project CHIP Authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "LiveViewCommand.h"

#include <commands/clusters/SubscriptionsCommands.h>
#include <commands/common/Commands.h>
#include <commands/discover/Commands.h>
#include <commands/example/ExampleCredentialIssuerCommands.h>
#include <commands/icd/ICDCommand.h>
#include <commands/pairing/Commands.h>
#include <commands/session-management/Commands.h>
#include <commands/storage/Commands.h>
#include <zap-generated/cluster/Commands.h>

#include <cstring>

int main(int argc, char * argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--media-self-test") == 0)
    {
        return CameraController::RunMediaSelfTest();
    }
    ExampleCredentialIssuerCommands credentials;
    Commands commands;
    registerCommandsICD(commands, &credentials);
    registerCommandsPairing(commands, &credentials);
    registerCommandsDiscover(commands, &credentials);
    registerCommandsStorage(commands);
    registerCommandsSessionManagement(commands, &credentials);
    registerClusters(commands, &credentials);
    registerCommandsSubscriptions(commands, &credentials);
    commands_list liveView = { make_unique<CameraController::LiveViewCommand>(&credentials) };
    commands.RegisterCommandSet("liveview", liveView, "Native Windows receive-only H.264 live view.");
    return commands.Run(argc, argv);
}

void testNodeCommandDirtyParity(Expectations& expectations,
                                ProjectSessionIdentitySource& identities) {
    using namespace bloom;
    using namespace commands;
    for (int operation = 0; operation < 11; ++operation) {
        auto session = decodedSessionWithPath(expectations, identities);
        const auto decoded = session.decodedSnapshot();
        const auto& snapshot = decoded.snapshot();
        const auto compositionId = snapshot.project().compositions().front().id();
        const auto execute = [&](std::unique_ptr<Operation> edit) {
            Transaction transaction("Node authoring", currentRevision(session));
            if (!transaction.add(std::move(edit)))
                throw std::logic_error("node dirty fixture operation");
            return session.execute(std::move(transaction));
        };
        const auto first = execute(std::make_unique<AddSolidLayer>(
            compositionId, "Layer", core::Color4d{1, 0, 0, 1}, document::Vec2d{}));
        if (!first.command)
            throw std::logic_error("dirty layer fixture");
        const auto source =
            first.command->outputId<document::NodeId>(kAddSolidLayerSolidNodeOutput);
        const auto boundary =
            first.command->outputId<document::NodeId>(kAddSolidLayerLayerOutputNodeOutput);
        const auto layer = first.command->outputId<document::LayerId>(kAddSolidLayerLayerOutput);
        const auto second = execute(std::make_unique<AddNode>(
            compositionId, std::string(document::kSolidSourceNodeType), document::Vec2d{}));
        if (!second.command)
            throw std::logic_error("dirty source fixture");
        const auto other = second.command->outputId<document::NodeId>(kAddNodeOutput);
        const auto loose = execute(std::make_unique<AddNode>(
            compositionId, std::string(document::kLayerOutputNodeType), document::Vec2d{}));
        if (!loose.command)
            throw std::logic_error("dirty boundary fixture");
        const auto processing = loose.command->outputId<document::NodeId>(kAddNodeOutput);
        if (!source || !boundary || !layer || !other || !processing)
            throw std::logic_error("dirty fixture IDs");
        if (!execute(std::make_unique<ConnectPorts>(
                         compositionId, document::OutputPortRef{*source, "image"},
                         document::InputPortRef{document::NodeInputRef{*processing, "image"}}))
                 .changed() ||
            !execute(std::make_unique<ConnectPorts>(
                         compositionId, document::OutputPortRef{*processing, "image"},
                         document::InputPortRef{document::NodeInputRef{*boundary, "image"}}))
                 .changed())
            throw std::logic_error("dirty fixture wiring");
        const auto before = currentRevision(session);
        if (session.acceptSavepoint(session.capturePlainSavePathIntent(), publicationIntent(1),
                                    before) != ProjectSessionSavepointStatus::Accepted)
            throw std::logic_error("node clean savepoint");
        expectations.expect(session.stateSnapshot().dirty == false,
                            "node command starts at a clean savepoint");
        const auto history = session.stateSnapshot().historySize;
        std::unique_ptr<Operation> edit;
        switch (operation) {
        case 0:
            edit = std::make_unique<AddNode>(
                compositionId, std::string(document::kSolidSourceNodeType), document::Vec2d{3, 4});
            break;
        case 1:
            edit = std::make_unique<RemoveNodes>(compositionId, std::set{*other});
            break;
        case 2:
            edit = std::make_unique<DuplicateNodes>(compositionId, std::set{*source},
                                                    document::Vec2d{3, 4});
            break;
        case 3:
            edit = std::make_unique<RenameLayer>(compositionId, *layer, "Renamed");
            break;
        case 4:
            edit = std::make_unique<ConnectPorts>(
                compositionId, document::OutputPortRef{*other, "image"},
                document::InputPortRef{document::NodeInputRef{*processing, "image"}});
            break;
        case 5:
            edit = std::make_unique<DisconnectInput>(
                compositionId,
                document::InputPortRef{document::NodeInputRef{*processing, "image"}});
            break;
        case 6:
            edit = std::make_unique<DissolveNode>(compositionId, *processing);
            break;
        case 7:
            edit = std::make_unique<MoveNodes>(
                compositionId, std::map<document::NodeId, document::Vec2d>{{*source, {3, 4}}});
            break;
        case 8:
            edit = std::make_unique<SetNodeCollapsed>(compositionId, *source, true);
            break;
        case 9:
            edit = std::make_unique<SetNodeMuted>(compositionId, *source, true);
            break;
        case 10:
            edit = std::make_unique<SetNodeWidth>(compositionId, *source, 256);
            break;
        default:
            throw std::logic_error("node dirty operation index");
        }
        const auto result = execute(std::move(edit));
        expectations.expect(
            result.changed() && currentRevision(session).value() == before.value() + 1 &&
                session.stateSnapshot().historySize == history + 1 &&
                session.stateSnapshot().dirty == true,
            "each node command publishes one revision, one history entry, and dirty state");
        expectations.expect(session.undo().changed() &&
                                currentRevision(session).value() == before.value() + 2 &&
                                session.stateSnapshot().dirty == true,
                            "each node command undo keeps conservative revision/dirty parity");
        expectations.expect(session.redo().changed() &&
                                currentRevision(session).value() == before.value() + 3 &&
                                session.stateSnapshot().dirty == true,
                            "each node command redo keeps conservative revision/dirty parity");
    }
}

/**********************************************************************************/
/* This file is part of spla project                                              */
/* https://github.com/JetBrains-Research/spla                                     */
/**********************************************************************************/
/* MIT License                                                                    */
/*                                                                                */
/* Copyright (c) 2021 JetBrains-Research                                          */
/*                                                                                */
/* Permission is hereby granted, free of charge, to any person obtaining a copy   */
/* of this software and associated documentation files (the "Software"), to deal  */
/* in the Software without restriction, including without limitation the rights   */
/* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell      */
/* copies of the Software, and to permit persons to whom the Software is          */
/* furnished to do so, subject to the following conditions:                       */
/*                                                                                */
/* The above copyright notice and this permission notice shall be included in all */
/* copies or substantial portions of the Software.                                */
/*                                                                                */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR     */
/* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,       */
/* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE    */
/* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER         */
/* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,  */
/* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE  */
/* SOFTWARE.                                                                      */
/**********************************************************************************/

#include <expression/SplaExpressionManager.hpp>
#include <detail/SplaLibraryPrivate.hpp>
#include <detail/SplaError.hpp>
#include <numeric>
#include <vector>

#include <expression/matrix/SplaMatrixDataWrite.hpp>

spla::ExpressionManager::ExpressionManager(spla::Library &library) : mLibrary(library) {
    // Here we can register built-in processors, one by on
    // NOTE: order of registration matters. First one has priority almond others for the same op.
    Register(new MatrixDataWrite());
}

void spla::ExpressionManager::Submit(const spla::RefPtr<spla::Expression> &expression) {
    CHECK_RAISE_ERROR(expression.IsNotNull(), InvalidArgument, L"Passed null expression");
    CHECK_RAISE_ERROR(expression->GetState() == Expression::State::Default, InvalidArgument,
                      L"Passed expression=" << expression->GetLabel() << L" must be in `Default` state before evaluation");

    expression->SetState(Expression::State::Submitted);

    // NOTE: Special case, expression without nodes
    // Nothing to do here, return
    if (expression->Empty()) {
        expression->SetState(Expression::State::Evaluated);
        return;
    }

    ExpressionContext context;
    context.expression = expression;

    FindStartNodes(context);
    CHECK_RAISE_ERROR(!context.startNodes.empty(), InvalidArgument,
                      L"No start nodes to run computation in expression=" << expression->GetLabel()
                      << L"; Possibly have some dependency cycle?");

    FindEndNodes(context);
    CHECK_RAISE_ERROR(!context.endNodes.empty(), InvalidArgument,
                      L"No end nodes in expression=" << expression->GetLabel()
                      << "; Possibly have some dependency cycle?");

    CheckCycles(context);
    DefineTraversalPath(context);

    auto& traversal = context.traversal;
    auto& taskflow = context.taskflow;
    auto& nodesTaskflow = context.nodesTaskflow;
    auto& nodes = expression->GetNodes();

    nodesTaskflow.resize(nodes.size());

    for (auto idx: traversal) {
        // Select processor for node
        auto processor = SelectProcessor(idx, context);
        // Actually process node
        processor->Process(idx, context);
    }

    std::vector<tf::Task> modules;
    modules.reserve(nodes.size());

    for (auto& nodeTaskflow: nodesTaskflow) {
        // Build temporary modules to sync sub-flows
        modules.push_back(taskflow.composed_of(nodeTaskflow));
    }

    for (size_t idx: traversal) {
        // Compose final taskflow graph
        auto& node = nodes[idx];
        auto& next = node->GetNext();
        auto& thisModule = modules[idx];

        // For each child make thisModule -> nextModule dependency
        for (auto& n: next) {
            auto& nextModule = modules[n->GetIdx()];
            thisModule.precede(nextModule);
        }
    }

    auto& executor = mLibrary.GetPrivate().GetTaskFlowExecutor();
    auto future = executor.run(taskflow);

    // todo: check async completion
    future.wait();
    expression->SetState(Expression::State::Evaluated);
}

void spla::ExpressionManager::Register(const spla::RefPtr<spla::NodeProcessor> &processor) {
    CHECK_RAISE_ERROR(processor.IsNotNull(), InvalidArgument, L"Passed null processor");

    ExpressionNode::Operation op = processor->GetOperationType();
    auto list = mProcessors.find(op);

    if (list == mProcessors.end())
        list = mProcessors.emplace(op, ProcessorList()).first;

    list->second.push_back(processor);
}

void spla::ExpressionManager::FindStartNodes(spla::ExpressionContext &context) {
    auto& expression = context.expression;
    auto& nodes = expression->GetNodes();
    auto& start = context.startNodes;

    for (size_t idx = 0; idx < nodes.size(); idx++) {
        // No incoming dependency
        if (nodes[idx]->GetPrev().empty())
            start.push_back(idx);
    }
}

void spla::ExpressionManager::FindEndNodes(spla::ExpressionContext &context) {
    auto& expression = context.expression;
    auto& nodes = expression->GetNodes();
    auto& end = context.endNodes;

    for (size_t idx = 0; idx < nodes.size(); idx++) {
        // No incoming dependency
        if (nodes[idx]->GetNext().empty())
            end.push_back(idx);
    }
}

void spla::ExpressionManager::CheckCycles(spla::ExpressionContext &context) {
    auto& expression = context.expression;
    auto& nodes = expression->GetNodes();
    auto& start = context.startNodes;
    auto nodesCount = nodes.size();

    for (auto n: start) {
        std::vector<int> visited(nodesCount, 0);
        auto cycle = CheckCyclesImpl(n, visited, nodes);
        CHECK_RAISE_ERROR(!cycle, InvalidArgument, L"Provided expression has dependency cycle");
    }
}

bool spla::ExpressionManager::CheckCyclesImpl(size_t idx, std::vector<int> &visited,
                                              const std::vector<RefPtr<ExpressionNode>> &nodes) {
    if (visited[idx])
        return true;

    auto& node = nodes[idx];
    auto& next = node->GetNext();

    for (const auto& n: next) {
        if (CheckCyclesImpl(n->GetIdx(), visited, nodes))
            return true;
    }

    return false;
}

void spla::ExpressionManager::DefineTraversalPath(spla::ExpressionContext &context) {
    auto& expression = context.expression;
    auto& nodes = expression->GetNodes();
    auto& start = context.startNodes;
    auto nodesCount = nodes.size();

    size_t t = 0;
    std::vector<size_t> out(nodesCount, t);

    for (auto s: start)
        DefineTraversalPathImpl(s, t, out, nodes);

    using Pair = std::pair<size_t,size_t>;
    std::vector<Pair> traversal;

    traversal.reserve(nodesCount);
    for (size_t i = 0; i < nodesCount; i++)
        traversal.emplace_back(out[i], i);

    std::sort(traversal.begin(), traversal.end(), [](const Pair& a, const Pair& b) {
        return a.first > b.first;
    });

    context.traversal.resize(nodesCount);

    std::transform(traversal.begin(), traversal.end(), context.traversal.begin(), [](const Pair& a) -> size_t {
        return a.second;
    });
}

void spla::ExpressionManager::DefineTraversalPathImpl(size_t idx, size_t &t, std::vector<size_t> &out,
                                                      const std::vector<RefPtr<ExpressionNode>> &nodes) {
    if (!out[idx]) {
        t += 1;

        auto& node = nodes[idx];
        auto& next = node->GetNext();

        for (const auto& n: next)
            DefineTraversalPathImpl(n->GetIdx(), t, out, nodes);

        t += 1;

        out[idx] = t;
    }
}

spla::RefPtr<spla::NodeProcessor>
spla::ExpressionManager::SelectProcessor(size_t nodeIdx, spla::ExpressionContext &context) {
    auto& nodes = context.expression->GetNodes();
    auto& node = nodes[nodeIdx];
    ExpressionNode::Operation op = node->GetNodeOp();

    auto iter = mProcessors.find(op);

    CHECK_RAISE_ERROR(iter != mProcessors.end(), InvalidState,
                      L"No processors for such op=" << ExpressionNodeOpToStr(op));

    const auto& processors = iter->second;

    // NOTE: Iterate through all processors for this operation and
    // select the first one, which meets requirements
    for (auto& processor: processors)
        if (processor->Select(nodeIdx, context))
            return processor;

    RAISE_ERROR(InvalidState,
                L"Failed to find suitable processor for the node op=" << ExpressionNodeOpToStr(op));
}
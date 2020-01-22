#ifndef RUBY_TYPER_LSP_LSPTYPECHECKER_H
#define RUBY_TYPER_LSP_LSPTYPECHECKER_H

#include "ast/ast.h"
#include "common/concurrency/WorkerPool.h"
#include "common/kvstore/KeyValueStore.h"
#include "core/ErrorQueue.h"
#include "core/NameHash.h"
#include "core/core.h"
#include "main/lsp/LSPConfiguration.h"

namespace sorbet::core::lsp {
class PreemptionTaskManager;
}

namespace sorbet::realmain::lsp {

struct LSPQueryResult {
    std::vector<std::unique_ptr<core::lsp::QueryResponse>> responses;
    // (Optional) Error that occurred during the query that you can pass on to the client.
    std::unique_ptr<ResponseError> error = nullptr;
};

class TypecheckRun final {
public:
    // Errors encountered during typechecking.
    std::vector<std::unique_ptr<core::Error>> errors;
    // The set of files that were typechecked for errors.
    std::vector<core::FileRef> filesTypechecked;
    // The edit applied to `gs`.
    LSPFileUpdates updates;
    // Specifies if the typecheck run took the fast or slow path.
    bool tookFastPath = false;
    // If update took the slow path, contains a new global state that should be used moving forward.
    std::optional<std::unique_ptr<core::GlobalState>> newGS;

    TypecheckRun(std::vector<std::unique_ptr<core::Error>> errors = {},
                 std::vector<core::FileRef> filesTypechecked = {}, LSPFileUpdates updates = {},
                 bool tookFastPath = false, std::optional<std::unique_ptr<core::GlobalState>> newGS = std::nullopt);
};

/**
 * Encapsulates typechecker operations and enforces that they happen on a single thread.
 */
class LSPTypechecker final {
    /**
     * Contains the LSPTypechecker state that is needed to cancel a running slow path operation and any subsequent fast
     * path operations that have preempted it.
     */
    class UndoState final {
    public:
        // Stores the pre-slow-path global state.
        std::unique_ptr<core::GlobalState> gs;
        // Stores index trees containing data stored in `gs` that have been evicted during the slow path operation.
        UnorderedMap<int, ast::ParsedFile> evictedIndexed;
        // Stores file hashes that have been evicted during the slow path operation.
        UnorderedMap<int, core::FileHash> evictedFileHashes;
        // Stores the index trees stored in `gs` that were evicted because the slow path operation replaced `gs`.
        UnorderedMap<int, ast::ParsedFile> evictedIndexedFinalGS;
        // Stores the list of files that had errors before the slow path began.
        std::vector<core::FileRef> evictedFilesThatHaveErrors;

        UndoState(std::unique_ptr<core::GlobalState> oldGS, UnorderedMap<int, ast::ParsedFile> evictedIndexedFinalGS,
                  std::vector<core::FileRef> evictedFilesThatHaveErrors);

        /**
         * Records that the given items were evicted from LSPTypechecker following a typecheck run.
         */
        void recordEvictedState(ast::ParsedFile evictedIndexTree, core::FileHash evictedStateHash);
    };

    /** Contains the ID of the thread responsible for typechecking. */
    std::thread::id typecheckerThreadId;
    /** GlobalState used for typechecking. Mutable because typechecking routines, even when not changing the GlobalState
     * instance, actively consume and replace GlobalState. */
    mutable std::unique_ptr<core::GlobalState> gs;
    /** Trees that have been indexed (with initialGS) and can be reused between different runs */
    std::vector<ast::ParsedFile> indexed;
    /** Trees that have been indexed (with finalGS) and can be reused between different runs */
    UnorderedMap<int, ast::ParsedFile> indexedFinalGS;
    /** Hashes of global states obtained by resolving every file in isolation. Used for fastpath. */
    std::vector<core::FileHash> globalStateHashes;
    /** Stores the epoch in which we last sent diagnostics to the client for each file. Should be the same length as
     * globalStateHashes. */
    std::vector<u4> diagnosticEpochs;
    /** List of files that have had errors in last run*/
    std::vector<core::FileRef> filesThatHaveErrors;
    std::unique_ptr<KeyValueStore> kvstore; // always null for now.
    /** Set only when typechecking is happening on the slow path. Contains all of the state needed to restore
     * LSPTypechecker to its pre-slow-path state. */
    std::optional<UndoState> cancellationUndoState;

    std::shared_ptr<const LSPConfiguration> config;
    /** Used to preempt running slow paths. */
    std::shared_ptr<core::lsp::PreemptionTaskManager> preemptManager;
    /** Used for assertions. Indicates if `initialize` has been run. */
    bool initialized = false;

    /** Conservatively reruns entire pipeline without caching any trees. Returns 'true' if committed, 'false' if
     * canceled. */
    bool runSlowPath(LSPFileUpdates updates, WorkerPool &workers, bool cancelable,
                     std::optional<std::shared_ptr<core::lsp::PreemptionTaskManager>> preemptManager = std::nullopt);

    /** Runs incremental typechecking on the provided updates. */
    TypecheckRun runFastPath(LSPFileUpdates updates, WorkerPool &workers) const;

    /**
     * Sends diagnostics from a typecheck run to the client.
     * `epoch` specifies the epoch of the file updates that produced these diagnostics. Used to prevent emitting
     * outdated diagnostics from a slow path run if they had already been re-typechecked on the fast path.
     */
    void pushDiagnostics(u4 epoch, std::vector<core::FileRef> filesTypechecked,
                         std::vector<std::unique_ptr<core::Error>> errors);

    /** Commits the given file updates to LSPTypechecker. Does not send diagnostics. */
    void commitFileUpdates(LSPFileUpdates &updates, bool couldBeCanceled);

    /** Officially 'commits' the output of a `TypecheckRun` by updating the relevant state on LSPTypechecker and sending
     * diagnostics to the editor. */
    void commitTypecheckRun(TypecheckRun run);

    /**
     * Undoes the given slow path changes on LSPTypechecker, and clears the client's error list for any files that were
     * newly introduced with the canceled update. Returns a list of files that need to be retypechecked to update their
     * error lists.
     */
    std::vector<core::FileRef> restore(UndoState &undoState);

    /**
     * Get an LSPFileUpdates containing the latest versions of the given files. It's a "no-op" file update because it
     * doesn't actually change anything.
     */
    LSPFileUpdates getNoopUpdate(std::vector<core::FileRef> frefs) const;

public:
    /**
     * Computes state hashes for the given set of files. Does not require any typechecker state, so it is a static
     * method.
     */
    static std::vector<core::FileHash> computeFileHashes(const LSPConfiguration &config,
                                                         const std::vector<std::shared_ptr<core::File>> &files,
                                                         WorkerPool &workers);

    LSPTypechecker(std::shared_ptr<const LSPConfiguration> config,
                   std::shared_ptr<core::lsp::PreemptionTaskManager> preemptionTaskManager);
    ~LSPTypechecker() = default;

    /**
     * Conducts the first typechecking pass of the session, and initializes `gs`, `index`, and `globalStateHashes`
     * variables. Must be called before typecheck and other functions work.
     *
     * Writes all diagnostic messages to LSPOutput.
     */
    void initialize(LSPFileUpdates updates, WorkerPool &workers);

    /**
     * Typechecks the given input. Returns 'true' if the updates were committed, or 'false' if typechecking was
     * canceled. Distributes work across the given worker pool.
     */
    bool typecheck(LSPFileUpdates updates, WorkerPool &workers);

    /**
     * Re-typechecks the provided files to re-produce error messages.
     */
    TypecheckRun retypecheck(std::vector<core::FileRef> frefs, WorkerPool &workers) const;

    /** Runs the provided query against the given files, and returns matches. */
    LSPQueryResult query(const core::lsp::Query &q, const std::vector<core::FileRef> &filesForQuery,
                         WorkerPool &workers) const;

    /**
     * Returns the parsed file for the given file, up to the index passes (does not include resolver passes).
     */
    const ast::ParsedFile &getIndexed(core::FileRef fref) const;

    /**
     * Returns the parsed files for the given files, including resolver.
     */
    std::vector<ast::ParsedFile> getResolved(const std::vector<core::FileRef> &frefs) const;

    /**
     * Returns the hashes of all committed files.
     */
    const std::vector<core::FileHash> &getFileHashes() const;

    /**
     * Returns the currently active GlobalState.
     */
    const core::GlobalState &state() const;

    /**
     * Called by LSPTypecheckerCoordinator to indicate that typechecking will occur on the current thread.
     */
    void changeThread();

    /**
     * Returns the typechecker's internal global state, which effectively destroys the typechecker for further use.
     */
    std::unique_ptr<core::GlobalState> destroy();
};

/**
 * Provides lambdas with a set of operations that they are allowed to do with the LSPTypechecker.
 */
class LSPTypecheckerDelegate {
    LSPTypechecker &typechecker;

public:
    /** The WorkerPool on which work will be performed. If the task is multithreaded, the pool will contain multiple
     * worker threads. */
    WorkerPool &workers;

    /**
     * Creates a new delegate that runs LSPTypechecker operations on the WorkerPool threads.
     */
    LSPTypecheckerDelegate(WorkerPool &workers, LSPTypechecker &typechecker);

    // Delete copy constructor / assignment.
    LSPTypecheckerDelegate(LSPTypecheckerDelegate &) = delete;
    LSPTypecheckerDelegate(const LSPTypecheckerDelegate &) = delete;
    LSPTypecheckerDelegate &operator=(LSPTypecheckerDelegate &&) = delete;
    LSPTypecheckerDelegate &operator=(const LSPTypecheckerDelegate &) = delete;

    /**
     * Typechecks the given input on the fast path. The edit *must* be a fast path edit!
     */
    void typecheckOnFastPath(LSPFileUpdates updates);

    /**
     * Re-typechecks the provided files to re-produce error messages.
     */
    TypecheckRun retypecheck(std::vector<core::FileRef> frefs) const;

    /** Runs the provided query against the given files, and returns matches. */
    LSPQueryResult query(const core::lsp::Query &q, const std::vector<core::FileRef> &filesForQuery) const;

    /**
     * Returns the parsed file for the given file, up to the index passes (does not include resolver passes).
     */
    const ast::ParsedFile &getIndexed(core::FileRef fref) const;

    /**
     * Returns the parsed files for the given files, including resolver.
     */
    std::vector<ast::ParsedFile> getResolved(const std::vector<core::FileRef> &frefs) const;

    /**
     * Returns the hashes of all committed files.
     */
    const std::vector<core::FileHash> &getFileHashes() const;

    /**
     * Returns the currently active GlobalState.
     */
    const core::GlobalState &state() const;
};

} // namespace sorbet::realmain::lsp
#endif

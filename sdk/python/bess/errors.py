# SPDX-License-Identifier: BSD-3-Clause
"""Typed errors for the BESS control SDK (D-146).

Every failure the protocol can produce has its own type, so a caller can tell
them apart without reading a message: the daemon refused the request, the
generation moved under it, operations were rejected, the daemon was busy, the
daemon restarted (so a lost answer cannot be read as "never ran"), a resource
reference went stale, or the transport lost the answer.
"""


class Error(Exception):
    """Base class for every error this SDK raises."""


class InvalidRequestError(Error):
    """The daemon refused the request as invalid.

    `code` and `detail` carry the daemon's `ErrorDetail` when there was one:
    `errno_value`, `message`, `object`, `field` and `daemon_epoch`.
    """

    def __init__(self, message, code=None, detail=None):
        super().__init__(message)
        self.code = code
        self.detail = detail


class OpFailure:
    """One operation's failure inside a rejected transaction."""

    __slots__ = ("index", "error")

    def __init__(self, index, error):
        self.index = index
        self.error = error

    def __repr__(self):
        return f"OpFailure(index={self.index}, error={self.error!r})"


class RejectedError(Error):
    """The transaction was rejected: some of its operations failed.

    `record` is the daemon's `TransactionRecord` and `failures` the per-op
    failures, so a caller can see exactly which operation was refused.
    """

    def __init__(self, message, record=None, failures=None):
        super().__init__(message)
        self.record = record
        self.failures = list(failures or [])


class ConflictError(Error):
    """The expected generation (or epoch) did not match the daemon's.

    `after_unknown_attempt` is True when the conflict was discovered after an
    attempt whose outcome is unknown, so the caller must re-read rather than
    assume the transaction did not run.
    """

    def __init__(self, message, record=None, after_unknown_attempt=False):
        super().__init__(message)
        self.record = record
        self.after_unknown_attempt = after_unknown_attempt


class BusyError(Error):
    """The daemon was busy and the attempt budget ran out."""

    def __init__(self, message, attempts=0, detail=None):
        super().__init__(message)
        self.attempts = attempts
        self.detail = detail


class TransportError(Error):
    """The transport lost the answer: the request may or may not have run.

    `request_id` is the transaction's, so the caller can ask `GetTransaction`
    what happened instead of guessing.
    """

    def __init__(self, message, request_id="", code=None):
        super().__init__(message)
        self.request_id = request_id
        self.code = code


class StaleResourceError(Error):
    """A resource reference no longer names what it did (an old snapshot)."""


class DaemonRestartedError(Error):
    """The daemon restarted, so a lost outcome is unknown, not "never ran".

    `refused` is True when the request was refused outright rather than
    accepted and then lost.
    """

    def __init__(self, message, refused=False):
        super().__init__(message)
        self.refused = refused

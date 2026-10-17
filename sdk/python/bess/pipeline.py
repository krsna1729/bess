# SPDX-License-Identifier: BSD-3-Clause
"""Desired-state helpers: versions, snapshots, and the builder (D-146)."""

from typing import NamedTuple, Optional

from .controlv2 import control_v2_pb2 as v2


class PipelineVersion(NamedTuple):
    """The version every pipeline answer carries.

    Three counters, and they are not interchangeable. `generation` moves when
    the pipeline's shape changes; `resource_generation` moves when a resource
    changes, and it is the one a transaction's `expected_generation` compares
    against (control_v2.proto: "the dataplane transaction generation"). The
    daemon epoch says whether the daemon restarted underneath.
    """

    generation: int
    daemon_epoch: int
    resource_generation: int = 0


class PipelineSnapshot:
    """A pipeline as the daemon has it, with the version it was read at."""

    __slots__ = ("pipeline", "version")

    def __init__(self, pipeline, version):
        self.pipeline = pipeline
        self.version = version

    def __repr__(self):
        return (
            f"PipelineSnapshot(modules={len(self.pipeline.modules)}, "
            f"connections={len(self.pipeline.connections)}, "
            f"resources={len(self.pipeline.resources)}, version={self.version!r})"
        )


class Resource:
    """One entry of `ListTransactionResources`: a nameable resource.

    `name` is what a transaction op names ("<module>/<table>"); `key_type` and
    `value_type` are the message type names the daemon expects, which is what
    makes a wrong resource a client-side error rather than a daemon error.
    """

    __slots__ = ("name", "key_type", "value_type", "kind", "requires_paused_workers")

    def __init__(self, name, key_type="", value_type="", kind="", requires_paused_workers=False):
        self.name = name
        self.key_type = key_type
        self.value_type = value_type
        self.kind = kind
        self.requires_paused_workers = requires_paused_workers

    def __repr__(self):
        return f"Resource({self.name!r}, {self.key_type} -> {self.value_type})"


class PipelineBuilder:
    """Builds a `Pipeline` by name, the way the other SDKs do.

    Start from a snapshot's pipeline to modify it, or from nothing to write a
    new one. `build()` returns a copy, so a builder stays usable after it.
    """

    def __init__(self, pipeline=None):
        self._pipeline = v2.Pipeline()
        if pipeline is not None:
            self._pipeline.CopyFrom(pipeline)

    def module(self, name, mclass, arg=None):
        """Adds a module. `arg` is a protobuf message packed into an Any."""
        entry = self._pipeline.modules.add()
        entry.name = name
        entry.mclass = mclass
        if arg is not None:
            entry.arg.Pack(arg)
        return self

    def connect(self, upstream, downstream, ogate=0, igate=0, skip_default_hooks=False):
        """Connects one output gate to one input gate."""
        entry = self._pipeline.connections.add()
        entry.upstream = upstream
        entry.ogate = ogate
        entry.downstream = downstream
        entry.igate = igate
        entry.skip_default_hooks = skip_default_hooks
        return self

    def worker(self, wid, core, scheduler=""):
        entry = self._pipeline.workers.add()
        entry.wid = wid
        entry.core = core
        if scheduler:
            entry.scheduler = scheduler
        return self

    def traffic_class(
        self,
        name,
        policy,
        parent="",
        resource="",
        wid=-1,
        priority=None,
        share=None,
        limit=None,
        max_burst=None,
    ):
        entry = self._pipeline.traffic_classes.add()
        entry.name = name
        entry.policy = policy
        entry.parent = parent
        entry.resource = resource
        entry.wid = wid
        if priority is not None:
            entry.priority = priority
        if share is not None:
            entry.share = share
        if limit:
            entry.limit.update(limit)
        if max_burst:
            entry.max_burst.update(max_burst)
        return self

    def task(self, module, task=0, parent="", wid=None, priority=None, share=None):
        entry = self._pipeline.tasks.add()
        entry.module = module
        entry.task = task
        entry.parent = parent
        if wid is not None:
            entry.wid = wid
        if priority is not None:
            entry.priority = priority
        if share is not None:
            entry.share = share
        return self

    def remove(self, name):
        """Removes a module, its connections and its task placements by name."""
        kept = [m for m in self._pipeline.modules if m.name != name]
        del self._pipeline.modules[:]
        self._pipeline.modules.extend(kept)

        connections = [
            c
            for c in self._pipeline.connections
            if c.upstream != name and c.downstream != name
        ]
        del self._pipeline.connections[:]
        self._pipeline.connections.extend(connections)

        tasks = [t for t in self._pipeline.tasks if t.module != name]
        del self._pipeline.tasks[:]
        self._pipeline.tasks.extend(tasks)
        return self

    def build(self) -> v2.Pipeline:
        out = v2.Pipeline()
        out.CopyFrom(self._pipeline)
        return out

    def __repr__(self):
        return (
            f"PipelineBuilder(modules={len(self._pipeline.modules)}, "
            f"connections={len(self._pipeline.connections)})"
        )

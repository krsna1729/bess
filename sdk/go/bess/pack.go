// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"sync"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/known/anypb"
)

// packer is how one message type goes into an Any, worked out once per type
// rather than once per operation: its type URL, and whether marshalling may
// skip the required-field check. Only a proto2 type can declare a required
// field. For a type that reaches none, the check is a walk of the message that
// can find nothing, and on a dynamic message (a schema-driven controller's) it
// allocates for every message it visits.
type packer struct {
	md      protoreflect.MessageDescriptor
	url     string
	partial bool
}

// packers holds one packer per type name. A descriptor of the same name from
// a newer schema replaces the older one, so the map is bounded by the type
// names a process uses, not by how often it refreshes a schema.
var packers struct {
	sync.RWMutex
	byName map[protoreflect.FullName]*packer
}

func packerOf(md protoreflect.MessageDescriptor) *packer {
	name := md.FullName()
	packers.RLock()
	p := packers.byName[name]
	packers.RUnlock()
	if p != nil && p.md == md {
		return p
	}
	// anypb.New's URL: the prefix every protobuf runtime resolves.
	p = &packer{md: md, url: "type.googleapis.com/" + string(name), partial: !reachesRequired(md, nil)}
	packers.Lock()
	if packers.byName == nil {
		packers.byName = map[protoreflect.FullName]*packer{}
	}
	packers.byName[name] = p
	packers.Unlock()
	return p
}

// reachesRequired is whether a message of type md can hold a required field
// unset: one of its own, or one of any message type it can contain, maps'
// values included. A type with extension ranges counts as reaching one,
// because the extensions a message carries are not in its descriptor.
func reachesRequired(md protoreflect.MessageDescriptor, seen map[protoreflect.FullName]bool) bool {
	if md.RequiredNumbers().Len() > 0 || md.ExtensionRanges().Len() > 0 {
		return true
	}
	if seen[md.FullName()] {
		return false // already being walked: a cycle adds no new field
	}
	if seen == nil {
		seen = map[protoreflect.FullName]bool{}
	}
	seen[md.FullName()] = true
	fields := md.Fields()
	for i := range fields.Len() {
		if sub := fields.Get(i).Message(); sub != nil && reachesRequired(sub, seen) {
			return true
		}
	}
	return false
}

// pack is anypb.New into a, which the caller allocates (beside the operation
// that holds it, as Upsert does). It refuses exactly what anypb.New refuses: a
// required field unset anywhere in m.
func pack(a *anypb.Any, m proto.Message) error {
	p := packerOf(m.ProtoReflect().Descriptor())
	b, err := proto.MarshalOptions{AllowPartial: p.partial}.Marshal(m)
	if err != nil {
		return err
	}
	a.TypeUrl, a.Value = p.url, b
	return nil
}

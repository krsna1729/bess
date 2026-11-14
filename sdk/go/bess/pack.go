// SPDX-License-Identifier: BSD-3-Clause

package bess

import (
	"strings"
	"sync"
	"weak"

	"google.golang.org/protobuf/proto"
	"google.golang.org/protobuf/reflect/protoreflect"
	"google.golang.org/protobuf/types/known/anypb"
)

const (
	maxTypeCacheNames = 4096
	maxTypeCacheBytes = 1 << 20
	typeURLPrefix     = "type.googleapis.com/"
)

// packer memoizes how one message type goes into an Any while its weak cache
// entry lives: its type URL, and whether marshalling may
// skip the required-field check. Only a proto2 type can declare a required
// field. For a type that reaches none, the check is a walk of the message that
// can find nothing, and on a dynamic message (a schema-driven controller's) it
// allocates for every message it visits.
type packer struct {
	md      protoreflect.MessageDescriptor
	url     string
	partial bool
}

// Packing is optional memoization, not an owner of dynamic schema graphs.
// Weak entries may expire at collection; bounded, owned names cannot retain
// a large descriptor/string backing allocation after a schema refresh.
var packers struct {
	sync.RWMutex
	byName map[protoreflect.FullName]weak.Pointer[packer]
	bytes  int // owned name bytes; map metadata is bounded by maxTypeCacheNames
}

func packerOf(md protoreflect.MessageDescriptor) *packer {
	name := md.FullName()
	packers.RLock()
	p := packers.byName[name].Value()
	packers.RUnlock()
	if p != nil && p.md == md {
		return p
	}
	// Descriptor walks stay outside the global cache lock: a cold type must
	// not hold up unrelated warm types.
	p = &packer{md: md, url: typeURL(string(name)), partial: !reachesRequired(md, nil)}
	if len(name) > maxTypeCacheBytes {
		return p
	}
	// Clone replacements too: assigning an equal string key can replace the
	// map's stored key with a view into the caller's much larger allocation.
	owned := protoreflect.FullName(strings.Clone(string(name)))
	packers.Lock()
	defer packers.Unlock()
	if current := packers.byName[name].Value(); current != nil && current.md == md {
		return current
	}
	if _, exists := packers.byName[name]; !exists &&
		(len(packers.byName) >= maxTypeCacheNames || packers.bytes+len(name) > maxTypeCacheBytes) {
		for key, entry := range packers.byName {
			if entry.Value() == nil {
				delete(packers.byName, key)
				packers.bytes -= len(key)
			}
		}
	}
	if _, exists := packers.byName[name]; !exists {
		if len(packers.byName) >= maxTypeCacheNames || packers.bytes+len(name) > maxTypeCacheBytes {
			return p // uncached packing still performs the full validation
		}
		packers.bytes += len(name)
	}
	if packers.byName == nil {
		packers.byName = make(map[protoreflect.FullName]weak.Pointer[packer])
	}
	packers.byName[owned] = weak.Make(p)
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

// typeURLs keeps bounded owned names and URLs. A full cache merely computes an
// uncached URL; it never changes the wire or refuses an otherwise valid request.
var typeURLs struct {
	sync.RWMutex
	byName map[string]string
	bytes  int
}

func typeURL(name string) string {
	if len(name) > (maxTypeCacheBytes-len(typeURLPrefix))/2 {
		return typeURLPrefix + name
	}
	typeURLs.RLock()
	url, ok := typeURLs.byName[name]
	cost := 0
	room := false
	if !ok {
		cost = 2*len(name) + len(typeURLPrefix)
		room = len(typeURLs.byName) < maxTypeCacheNames && typeURLs.bytes+cost <= maxTypeCacheBytes
	}
	typeURLs.RUnlock()
	if ok {
		return url
	}
	url = typeURLPrefix + name
	if !room {
		return url
	}
	owned := strings.Clone(name)
	typeURLs.Lock()
	if cached, exists := typeURLs.byName[name]; exists {
		url = cached
	} else if len(typeURLs.byName) < maxTypeCacheNames && typeURLs.bytes+cost <= maxTypeCacheBytes {
		if typeURLs.byName == nil {
			typeURLs.byName = make(map[string]string)
		}
		typeURLs.byName[owned] = url
		typeURLs.bytes += cost
	}
	typeURLs.Unlock()
	return url
}

/*
 * Copyright (c) 2016, 2019, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "classfile/systemDictionary.hpp"
#include "jfr/jfr.hpp"
#include "jfr/jni/jfrGetAllEventClasses.hpp"
#include "jfr/recorder/checkpoint/jfrCheckpointWriter.hpp"
#include "jfr/recorder/checkpoint/constant/jfrTagSet.hpp"
#include "jfr/recorder/checkpoint/constant/jfrTagSetUtils.hpp"
#include "jfr/recorder/checkpoint/constant/jfrTagSetWriter.hpp"
#include "jfr/recorder/checkpoint/constant/traceid/jfrTraceId.inline.hpp"
#include "jfr/recorder/storage/jfrBuffer.hpp"
#include "jfr/utilities/jfrHashtable.hpp"
#include "memory/iterator.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "memory/resourceArea.hpp"
// to get CONTENT_TYPE defines
#include "tracefiles/traceTypes.hpp"
#include "utilities/accessFlags.hpp"

// incremented on each checkpoint
static u8 checkpoint_id = 0;

// creates a unique id by combining a checkpoint relative symbol id (2^24)
// with the current checkpoint id (2^40)
#define CREATE_SYMBOL_ID(sym_id) (((u8)((checkpoint_id << 24) | sym_id)))

typedef const Klass* KlassPtr;
typedef const ClassLoaderData* CldPtr;
typedef const Method* MethodPtr;
typedef const Symbol* SymbolPtr;
typedef const JfrSymbolId::SymbolEntry* SymbolEntryPtr;
typedef const JfrSymbolId::CStringEntry* CStringEntryPtr;

static traceid cld_id(CldPtr cld) {
  assert(cld != NULL, "invariant");
  return cld->is_anonymous() ? 0 : TRACE_ID(cld);
}

static void tag_leakp_klass_artifacts(KlassPtr k, bool class_unload) {
  assert(k != NULL, "invariant");
  CldPtr cld = k->class_loader_data();
  assert(cld != NULL, "invariant");
  if (!cld->is_anonymous()) {
    tag_leakp_artifact(cld, class_unload);
  }
}

class TagLeakpKlassArtifact {
  bool _class_unload;
 public:
  TagLeakpKlassArtifact(bool class_unload) : _class_unload(class_unload) {}
  bool operator()(KlassPtr klass) {
    if (_class_unload) {
      if (LEAKP_USED_THIS_EPOCH(klass)) {
        tag_leakp_klass_artifacts(klass, _class_unload);
      }
    } else {
      if (LEAKP_USED_PREV_EPOCH(klass)) {
        tag_leakp_klass_artifacts(klass, _class_unload);
      }
    }
    return true;
  }
};

/*
 * In C++03, functions used as template parameters must have external linkage;
 * this restriction was removed in C++11. Change back to "static" and
 * rename functions when C++11 becomes available.
 *
 * The weird naming is an effort to decrease the risk of name clashes.
 */

int write__artifact__klass(JfrCheckpointWriter* writer, JfrArtifactSet* artifacts, const void* k) {
  assert(writer != NULL, "invariant");
  assert(artifacts != NULL, "invariant");
  assert(k != NULL, "invariant");
  KlassPtr klass = (KlassPtr)k;
  traceid pkg_id = 0;
  KlassPtr theklass = klass;
  if (theklass->oop_is_objArray()) {
    const ObjArrayKlass* obj_arr_klass = ObjArrayKlass::cast(klass);
    theklass = obj_arr_klass->bottom_klass();
  }
  if (theklass->oop_is_instance()) {
  } else {
    assert(theklass->oop_is_typeArray(), "invariant");
  }
  const traceid symbol_id = artifacts->mark(klass);
  assert(symbol_id > 0, "need to have an address for symbol!");
  writer->write(TRACE_ID(klass));
  writer->write(cld_id(klass->class_loader_data()));
  writer->write((traceid)CREATE_SYMBOL_ID(symbol_id));
  writer->write((s4)klass->access_flags().get_flags());
  return 1;
}

typedef LeakPredicate<KlassPtr> LeakKlassPredicate;
typedef JfrPredicatedArtifactWriterImplHost<KlassPtr, LeakKlassPredicate, write__artifact__klass> LeakKlassWriterImpl;
typedef JfrArtifactWriterHost<LeakKlassWriterImpl, CONSTANT_TYPE_CLASS> LeakKlassWriter;
typedef JfrArtifactWriterImplHost<KlassPtr, write__artifact__klass> KlassWriterImpl;
typedef JfrArtifactWriterHost<KlassWriterImpl, CONSTANT_TYPE_CLASS> KlassWriter;

int write__artifact__method(JfrCheckpointWriter* writer, JfrArtifactSet* artifacts, const void* m) {
  assert(writer != NULL, "invariant");
  assert(artifacts != NULL, "invariant");
  assert(m != NULL, "invariant");
  MethodPtr method = (MethodPtr)m;
  const traceid method_name_symbol_id = artifacts->mark(method->name());
  assert(method_name_symbol_id > 0, "invariant");
  const traceid method_sig_symbol_id = artifacts->mark(method->signature());
  assert(method_sig_symbol_id > 0, "invariant");
  KlassPtr klass = method->method_holder();
  assert(klass != NULL, "invariant");
  assert(METHOD_USED_ANY_EPOCH(klass), "invariant");
  writer->write((u8)METHOD_ID(klass, method));
  writer->write((u8)TRACE_ID(klass));
  writer->write((u8)CREATE_SYMBOL_ID(method_name_symbol_id));
  writer->write((u8)CREATE_SYMBOL_ID(method_sig_symbol_id));
  writer->write((u2)method->access_flags().get_flags());
  writer->write(const_cast<Method*>(method)->is_hidden() ? (u1)1 : (u1)0);
  return 1;
}

typedef JfrArtifactWriterImplHost<MethodPtr, write__artifact__method> MethodWriterImplTarget;
typedef JfrArtifactWriterHost<MethodWriterImplTarget, CONSTANT_TYPE_METHOD> MethodWriterImpl;

int write__artifact__classloader(JfrCheckpointWriter* writer, JfrArtifactSet* artifacts, const void* c) {
  assert(c != NULL, "invariant");
  CldPtr cld = (CldPtr)c;
  assert(!cld->is_anonymous(), "invariant");
  const traceid cld_id = TRACE_ID(cld);
  // class loader type
  const oop class_loader_oop = cld->class_loader();
  if (class_loader_oop == NULL) {
    // (primordial) boot class loader
    writer->write(cld_id); // class loader instance id
    writer->write((traceid)0);  // class loader type id (absence of)
    writer->write((traceid)CREATE_SYMBOL_ID(1)); // 1 maps to synthetic name -> "boot"
    return 1;
  }
  assert(class_loader_oop != NULL, "invariant");
  KlassPtr class_loader_klass = class_loader_oop->klass();
  traceid symbol_name_id = 0;
  const oop class_loader_name_oop = NULL;
  if (class_loader_name_oop != NULL) {
    const char* class_loader_instance_name =
      java_lang_String::as_utf8_string(class_loader_name_oop);
    if (class_loader_instance_name != NULL && class_loader_instance_name[0] != '\0') {
      // tag the symbol as "anonymous" since its not really a Symbol* but a const char*,
      // it will be handled correctly if it has the anonymous tag on insertion
      symbol_name_id = artifacts->mark(class_loader_instance_name,
                                       java_lang_String::hash_code(class_loader_name_oop));
    }
  }
  writer->write(cld_id); // class loader instance id
  writer->write(TRACE_ID(class_loader_klass)); // class loader type id
  writer->write(symbol_name_id == 0 ? (traceid)0 :
                 (traceid)CREATE_SYMBOL_ID(symbol_name_id)); // class loader instance name
  return 1;
}

typedef LeakPredicate<CldPtr> LeakCldPredicate;
int _compare_cld_ptr_(CldPtr const& lhs, CldPtr const& rhs) { return lhs > rhs ? 1 : (lhs < rhs) ? -1 : 0; }
typedef UniquePredicate<CldPtr, _compare_cld_ptr_> CldPredicate;
typedef JfrPredicatedArtifactWriterImplHost<CldPtr, LeakCldPredicate, write__artifact__classloader> LeakCldWriterImpl;
typedef JfrPredicatedArtifactWriterImplHost<CldPtr, CldPredicate, write__artifact__classloader> CldWriterImpl;
typedef JfrArtifactWriterHost<LeakCldWriterImpl, CONSTANT_TYPE_CLASSLOADER> LeakCldWriter;
typedef JfrArtifactWriterHost<CldWriterImpl, CONSTANT_TYPE_CLASSLOADER> CldWriter;

typedef const JfrSymbolId::SymbolEntry* SymbolEntryPtr;

static int write__artifact__symbol__entry__(JfrCheckpointWriter* writer,
                                            SymbolEntryPtr entry) {
  assert(writer != NULL, "invariant");
  assert(entry != NULL, "invariant");
  ResourceMark rm;
  writer->write(CREATE_SYMBOL_ID(entry->id()));
  writer->write(entry->value()->as_C_string());
  return 1;
}

int write__artifact__symbol__entry(JfrCheckpointWriter* writer, JfrArtifactSet* artifacts, const void* e) {
  assert(e != NULL, "invariant");
  return write__artifact__symbol__entry__(writer, (SymbolEntryPtr)e);
}

typedef JfrArtifactWriterImplHost<SymbolEntryPtr, write__artifact__symbol__entry> SymbolEntryWriterImpl;
typedef JfrArtifactWriterHost<SymbolEntryWriterImpl, CONSTANT_TYPE_SYMBOL> SymbolEntryWriter;

typedef const JfrSymbolId::CStringEntry* CStringEntryPtr;

static int write__artifact__cstring__entry__(JfrCheckpointWriter* writer, CStringEntryPtr entry) {
  assert(writer != NULL, "invariant");
  assert(entry != NULL, "invariant");
  writer->write(CREATE_SYMBOL_ID(entry->id()));
  writer->write(entry->value());
  return 1;
}

int write__artifact__cstring__entry(JfrCheckpointWriter* writer, JfrArtifactSet* artifacts, const void* e) {
  assert(e != NULL, "invariant");
  return write__artifact__cstring__entry__(writer, (CStringEntryPtr)e);
}

typedef JfrArtifactWriterImplHost<CStringEntryPtr, write__artifact__cstring__entry> CStringEntryWriterImpl;
typedef JfrArtifactWriterHost<CStringEntryWriterImpl, CONSTANT_TYPE_SYMBOL> CStringEntryWriter;

int write__artifact__klass__symbol(JfrCheckpointWriter* writer, JfrArtifactSet* artifacts, const void* k) {
  assert(writer != NULL, "invariant");
  assert(artifacts != NULL, "invaiant");
  assert(k != NULL, "invariant");
  const InstanceKlass* const ik = (const InstanceKlass*)k;
  if (ik->is_anonymous()) {
    CStringEntryPtr entry =
      artifacts->map_cstring(JfrSymbolId::anonymous_klass_name_hash_code(ik));
    assert(entry != NULL, "invariant");
    return write__artifact__cstring__entry__(writer, entry);
  }

  SymbolEntryPtr entry = artifacts->map_symbol(JfrSymbolId::regular_klass_name_hash_code(ik));
  return write__artifact__symbol__entry__(writer, entry);
}

int _compare_traceid_(const traceid& lhs, const traceid& rhs) {
  return lhs > rhs ? 1 : (lhs < rhs) ? -1 : 0;
}

template <template <typename> class Predicate>
class KlassSymbolWriterImpl {
 private:
  JfrCheckpointWriter* _writer;
  JfrArtifactSet* _artifacts;
  Predicate<KlassPtr> _predicate;
  MethodUsedPredicate<true> _method_used_predicate;
  MethodFlagPredicate _method_flag_predicate;
  UniquePredicate<traceid, _compare_traceid_> _unique_predicate;

  int klass_symbols(KlassPtr klass);
  int class_loader_symbols(CldPtr cld);
  int method_symbols(KlassPtr klass);

 public:
  typedef KlassPtr Type;
  KlassSymbolWriterImpl(JfrCheckpointWriter* writer,
                        JfrArtifactSet* artifacts,
                        bool class_unload) : _writer(writer),
                                             _artifacts(artifacts),
                                             _predicate(class_unload),
                                             _method_used_predicate(class_unload),
                                             _method_flag_predicate(class_unload),
                                             _unique_predicate(class_unload) {}

  int operator()(KlassPtr klass) {
    assert(klass != NULL, "invariant");
    int count = 0;
    if (_predicate(klass)) {
      count += klass_symbols(klass);
      CldPtr cld = klass->class_loader_data();
      assert(cld != NULL, "invariant");
      if (!cld->is_anonymous()) {
        count += class_loader_symbols(cld);
      }
      if (_method_used_predicate(klass)) {
        count += method_symbols(klass);
      }
    }
    return count;
  }
};

template <template <typename> class Predicate>
int KlassSymbolWriterImpl<Predicate>::klass_symbols(KlassPtr klass) {
  assert(klass != NULL, "invariant");
  assert(_predicate(klass), "invariant");
  const InstanceKlass* const ik = (const InstanceKlass*)klass;
  if (ik->is_anonymous()) {
    CStringEntryPtr entry =
      this->_artifacts->map_cstring(JfrSymbolId::anonymous_klass_name_hash_code(ik));
    assert(entry != NULL, "invariant");
    return _unique_predicate(entry->id()) ? write__artifact__cstring__entry__(this->_writer, entry) : 0;
  }
  SymbolEntryPtr entry = this->_artifacts->map_symbol(ik->name());
  assert(entry != NULL, "invariant");
  return _unique_predicate(entry->id()) ? write__artifact__symbol__entry__(this->_writer, entry) : 0;
}

template <template <typename> class Predicate>
int KlassSymbolWriterImpl<Predicate>::class_loader_symbols(CldPtr cld) {
  assert(cld != NULL, "invariant");
  assert(!cld->is_anonymous(), "invariant");
  CStringEntryPtr entry = NULL;
  // class loader type
  const oop class_loader_oop = cld->class_loader();
  if (class_loader_oop == NULL) {
    // (primordial) boot class loader
    entry = this->_artifacts->map_cstring(0);
    assert(entry != NULL, "invariant");
    assert(strncmp(entry->literal(),
                   boot_class_loader_name,
                   strlen(boot_class_loader_name)) == 0, "invariant");
  } else {
    assert(class_loader_oop != NULL, "invariant");
    KlassPtr class_loader_klass = class_loader_oop->klass();
    const oop class_loader_name_oop = NULL;
    if (class_loader_name_oop != NULL) {
      const char* class_loader_instance_name =
        java_lang_String::as_utf8_string(class_loader_name_oop);
      if (class_loader_instance_name != NULL && class_loader_instance_name[0] != '\0') {
        entry = this->_artifacts->map_cstring(java_lang_String::hash_code(class_loader_name_oop));
      }
    }
  }
  return entry == NULL ? 0 : _unique_predicate(entry->id()) ?
    write__artifact__cstring__entry__(this->_writer, entry) : 0;
}

template <template <typename> class Predicate>
int KlassSymbolWriterImpl<Predicate>::method_symbols(KlassPtr klass) {
  assert(_predicate(klass), "invariant");
  assert(_method_used_predicate(klass), "invariant");
  assert(METHOD_AND_CLASS_USED_ANY_EPOCH(klass), "invariant");
  int count = 0;
  const InstanceKlass* const ik = InstanceKlass::cast((Klass*)klass);
  const int len = ik->methods()->length();
  for (int i = 0; i < len; ++i) {
    MethodPtr method = ik->methods()->at(i);
    if (_method_flag_predicate(method)) {
      SymbolEntryPtr entry = this->_artifacts->map_symbol(method->name());
      assert(entry != NULL, "invariant");
      if (_unique_predicate(entry->id())) {
        count += write__artifact__symbol__entry__(this->_writer, entry);
      }
      entry = this->_artifacts->map_symbol(method->signature());
      assert(entry != NULL, "invariant");
      if (_unique_predicate(entry->id())) {
        count += write__artifact__symbol__entry__(this->_writer, entry);
      }
    }
  }
  return count;
}

typedef KlassSymbolWriterImpl<LeakPredicate> LeakKlassSymbolWriterImpl;
typedef JfrArtifactWriterHost<LeakKlassSymbolWriterImpl, CONSTANT_TYPE_SYMBOL> LeakKlassSymbolWriter;

class ClearKlassAndMethods {
 private:
  ClearArtifact<KlassPtr> _clear_klass_tag_bits;
  ClearArtifact<MethodPtr> _clear_method_flag;
  MethodUsedPredicate<false> _method_used_predicate;

 public:
  ClearKlassAndMethods(bool class_unload) : _clear_klass_tag_bits(class_unload),
                                            _clear_method_flag(class_unload),
                                            _method_used_predicate(class_unload) {}
  bool operator()(KlassPtr klass) {
    if (_method_used_predicate(klass)) {
      const InstanceKlass* ik = InstanceKlass::cast((Klass*)klass);
      const int len = ik->methods()->length();
      for (int i = 0; i < len; ++i) {
        MethodPtr method = ik->methods()->at(i);
        _clear_method_flag(method);
      }
    }
    _clear_klass_tag_bits(klass);
    return true;
  }
};

typedef CompositeFunctor<KlassPtr,
                         TagLeakpKlassArtifact,
                         LeakKlassWriter> LeakpKlassArtifactTagging;

typedef CompositeFunctor<KlassPtr,
                         LeakpKlassArtifactTagging,
                         KlassWriter> CompositeKlassWriter;

typedef CompositeFunctor<KlassPtr,
                         CompositeKlassWriter,
                         KlassArtifactRegistrator> CompositeKlassWriterRegistration;

typedef CompositeFunctor<KlassPtr,
                         KlassWriter,
                         KlassArtifactRegistrator> KlassWriterRegistration;

typedef JfrArtifactCallbackHost<KlassPtr, KlassWriterRegistration> KlassCallback;
typedef JfrArtifactCallbackHost<KlassPtr, CompositeKlassWriterRegistration> CompositeKlassCallback;

/*
 * Composite operation
 *
 * TagLeakpKlassArtifact ->
 *   LeakpPredicate ->
 *     LeakpKlassWriter ->
 *       KlassPredicate ->
 *         KlassWriter ->
 *           KlassWriterRegistration
 */
void JfrTagSet::write_klass_constants(JfrCheckpointWriter* writer, JfrCheckpointWriter* leakp_writer) {
  assert(!_artifacts->has_klass_entries(), "invariant");
  KlassArtifactRegistrator reg(_artifacts);
  KlassWriter kw(writer, _artifacts, _class_unload);
  KlassWriterRegistration kwr(&kw, &reg);
  if (leakp_writer == NULL) {
    KlassCallback callback(&kwr);
    _subsystem_callback = &callback;
    do_klasses();
    return;
  }
  TagLeakpKlassArtifact tagging(_class_unload);
  LeakKlassWriter lkw(leakp_writer, _artifacts, _class_unload);
  LeakpKlassArtifactTagging lpkat(&tagging, &lkw);
  CompositeKlassWriter ckw(&lpkat, &kw);
  CompositeKlassWriterRegistration ckwr(&ckw, &reg);
  CompositeKlassCallback callback(&ckwr);
  _subsystem_callback = &callback;
  do_klasses();
}

typedef CompositeFunctor<CldPtr, CldWriter, ClearArtifact<CldPtr> > CldWriterWithClear;
typedef CompositeFunctor<CldPtr, LeakCldWriter, CldWriter> CompositeCldWriter;
typedef CompositeFunctor<CldPtr, CompositeCldWriter, ClearArtifact<CldPtr> > CompositeCldWriterWithClear;
typedef JfrArtifactCallbackHost<CldPtr, CldWriterWithClear> CldCallback;
typedef JfrArtifactCallbackHost<CldPtr, CompositeCldWriterWithClear> CompositeCldCallback;

class CldFieldSelector {
 public:
  typedef CldPtr TypePtr;
  static TypePtr select(KlassPtr klass) {
    assert(klass != NULL, "invariant");
    CldPtr cld = klass->class_loader_data();
    return cld->is_anonymous() ? NULL : cld;
  }
};

typedef KlassToFieldEnvelope<CldFieldSelector, CldWriterWithClear> KlassCldWriterWithClear;
typedef KlassToFieldEnvelope<CldFieldSelector, CompositeCldWriterWithClear> KlassCompositeCldWriterWithClear;

/*
 * Composite operation
 *
 * LeakpClassLoaderWriter ->
 *   ClassLoaderWriter ->
 *     ClearArtifact<ClassLoaderData>
 */
void JfrTagSet::write_class_loader_constants(JfrCheckpointWriter* writer, JfrCheckpointWriter* leakp_writer) {
  assert(_artifacts->has_klass_entries(), "invariant");
  ClearArtifact<CldPtr> clear(_class_unload);
  CldWriter cldw(writer, _artifacts, _class_unload);
  if (leakp_writer == NULL) {
    CldWriterWithClear cldwwc(&cldw, &clear);
    KlassCldWriterWithClear kcldwwc(&cldwwc);
    _artifacts->iterate_klasses(kcldwwc);
    CldCallback callback(&cldwwc);
    _subsystem_callback = &callback;
    do_class_loaders();
    return;
  }
  LeakCldWriter lcldw(leakp_writer, _artifacts, _class_unload);
  CompositeCldWriter ccldw(&lcldw, &cldw);
  CompositeCldWriterWithClear ccldwwc(&ccldw, &clear);
  KlassCompositeCldWriterWithClear kcclwwc(&ccldwwc);
  _artifacts->iterate_klasses(kcclwwc);
  CompositeCldCallback callback(&ccldwwc);
  _subsystem_callback = &callback;
  do_class_loaders();
}

template <bool predicate_bool, typename MethodFunctor>
class MethodIteratorHost {
 private:
  MethodFunctor _method_functor;
  MethodUsedPredicate<predicate_bool> _method_used_predicate;
  MethodFlagPredicate _method_flag_predicate;

 public:
  MethodIteratorHost(JfrCheckpointWriter* writer,
                     JfrArtifactSet* artifacts,
                     bool class_unload,
                     bool skip_header = false) :
    _method_functor(writer, artifacts, class_unload, skip_header),
    _method_used_predicate(class_unload),
    _method_flag_predicate(class_unload) {}

  bool operator()(KlassPtr klass) {
    if (_method_used_predicate(klass)) {
      assert(METHOD_AND_CLASS_USED_ANY_EPOCH(klass), "invariant");
      const InstanceKlass* ik = InstanceKlass::cast((Klass*)klass);
      const int len = ik->methods()->length();
      for (int i = 0; i < len; ++i) {
        MethodPtr method = ik->methods()->at(i);
        if (_method_flag_predicate(method)) {
          _method_functor(method);
        }
      }
    }
    return true;
  }

  int count() const { return _method_functor.count(); }
  void add(int count) { _method_functor.add(count); }
};

typedef MethodIteratorHost<true /*leakp */,  MethodWriterImpl> LeakMethodWriter;
typedef MethodIteratorHost<false, MethodWriterImpl> MethodWriter;
typedef CompositeFunctor<KlassPtr, LeakMethodWriter, MethodWriter> CompositeMethodWriter;

/*
 * Composite operation
 *
 * LeakpMethodWriter ->
 *   MethodWriter
 */
void JfrTagSet::write_method_constants(JfrCheckpointWriter* writer, JfrCheckpointWriter* leakp_writer) {
  assert(_artifacts->has_klass_entries(), "invariant");
  MethodWriter mw(writer, _artifacts, _class_unload);
  if (leakp_writer == NULL) {
    _artifacts->iterate_klasses(mw);
    return;
  }
  LeakMethodWriter lpmw(leakp_writer, _artifacts, _class_unload);
  CompositeMethodWriter cmw(&lpmw, &mw);
  _artifacts->iterate_klasses(cmw);
}
static void write_symbols_leakp(JfrCheckpointWriter* leakp_writer, JfrArtifactSet* artifacts, bool class_unload) {
  assert(leakp_writer != NULL, "invariant");
  assert(artifacts != NULL, "invariant");
  LeakKlassSymbolWriter lpksw(leakp_writer, artifacts, class_unload);
  artifacts->iterate_klasses(lpksw);
}
static void write_symbols(JfrCheckpointWriter* writer, JfrCheckpointWriter* leakp_writer, JfrArtifactSet* artifacts, bool class_unload) {
  assert(writer != NULL, "invariant");
  assert(artifacts != NULL, "invariant");
  if (leakp_writer != NULL) {
    write_symbols_leakp(leakp_writer, artifacts, class_unload);
  }
  // iterate all registered symbols
  SymbolEntryWriter symbol_writer(writer, artifacts, class_unload);
  artifacts->iterate_symbols(symbol_writer);
  CStringEntryWriter cstring_writer(writer, artifacts, class_unload, true); // skip header
  artifacts->iterate_cstrings(cstring_writer);
  symbol_writer.add(cstring_writer.count());
}

bool JfrTagSet::_class_unload = false;
JfrArtifactSet* JfrTagSet::_artifacts = NULL;
JfrArtifactClosure* JfrTagSet::_subsystem_callback = NULL;

void JfrTagSet::write_symbol_constants(JfrCheckpointWriter* writer, JfrCheckpointWriter* leakp_writer) {
  assert(writer != NULL, "invariant");
  assert(_artifacts->has_klass_entries(), "invariant");
  write_symbols(writer, leakp_writer, _artifacts, _class_unload);
}

void JfrTagSet::do_unloaded_klass(Klass* klass) {
  assert(klass != NULL, "invariant");
  assert(_subsystem_callback != NULL, "invariant");
  if (IS_JDK_JFR_EVENT_SUBKLASS(klass)) {
    JfrEventClasses::increment_unloaded_event_class();
  }
  if (USED_THIS_EPOCH(klass)) { // includes leakp subset
    _subsystem_callback->do_artifact(klass);
    return;
  }
  if (klass->is_subclass_of(SystemDictionary::ClassLoader_klass()) || klass == SystemDictionary::Object_klass()) {
    SET_LEAKP_USED_THIS_EPOCH(klass); // tag leakp "safe byte" for subset inclusion
    _subsystem_callback->do_artifact(klass);
  }
}

void JfrTagSet::do_klass(Klass* klass) {
  assert(klass != NULL, "invariant");
  assert(_subsystem_callback != NULL, "invariant");
  if (USED_PREV_EPOCH(klass)) { // includes leakp subset
    _subsystem_callback->do_artifact(klass);
    return;
  }
  if (klass->is_subclass_of(SystemDictionary::ClassLoader_klass()) || klass == SystemDictionary::Object_klass()) {
    SET_LEAKP_USED_PREV_EPOCH(klass); // tag leakp "safe byte" for subset inclusion
    _subsystem_callback->do_artifact(klass);
  }
}

void JfrTagSet::do_klasses() {
  if (_class_unload) {
    ClassLoaderDataGraph::classes_unloading_do(&do_unloaded_klass);
    return;
  }
  ClassLoaderDataGraph::classes_do(&do_klass);
}

void JfrTagSet::do_unloaded_class_loader_data(ClassLoaderData* cld) {
  assert(_subsystem_callback != NULL, "invariant");
  if (ANY_USED_THIS_EPOCH(cld)) { // includes leakp subset
    _subsystem_callback->do_artifact(cld);
  }
}

void JfrTagSet::do_class_loader_data(ClassLoaderData* cld) {
  assert(_subsystem_callback != NULL, "invariant");
  if (ANY_USED_PREV_EPOCH(cld)) { // includes leakp subset
    _subsystem_callback->do_artifact(cld);
  }
}

class CLDCallback : public CLDClosure {
 private:
  bool _class_unload;
 public:
  CLDCallback(bool class_unload) : _class_unload(class_unload) {}
  void do_cld(ClassLoaderData* cld) {
     assert(cld != NULL, "invariant");
    if (cld->is_anonymous()) {
      return;
    }
    if (_class_unload) {
      JfrTagSet::do_unloaded_class_loader_data(cld);
      return;
    }
    JfrTagSet::do_class_loader_data(cld);
  }
};

void JfrTagSet::do_class_loaders() {
  CLDCallback cld_cb(_class_unload);
  if (_class_unload) {
    ClassLoaderDataGraph::cld_unloading_do(&cld_cb);
    return;
  }
  ClassLoaderDataGraph::cld_do(&cld_cb);
}

static void clear_artifacts(JfrArtifactSet* artifacts,
                            bool class_unload) {
  assert(artifacts != NULL, "invariant");
  assert(artifacts->has_klass_entries(), "invariant");

  // untag
  ClearKlassAndMethods clear(class_unload);
  artifacts->iterate_klasses(clear);
  artifacts->clear();
}

/**
 * Write all "tagged" (in-use) constant artifacts and their dependencies.
 */
void JfrTagSet::write(JfrCheckpointWriter* writer, JfrCheckpointWriter* leakp_writer, bool class_unload) {
  assert(writer != NULL, "invariant");
  ResourceMark rm;
  // initialization begin
  _class_unload = class_unload;
  ++checkpoint_id;
  if (_artifacts == NULL) {
    _artifacts = new JfrArtifactSet(class_unload);
    _subsystem_callback = NULL;
  } else {
    _artifacts->initialize(class_unload);
    _subsystem_callback = NULL;
  }
  assert(_artifacts != NULL, "invariant");
  assert(!_artifacts->has_klass_entries(), "invariant");
  assert(_subsystem_callback == NULL, "invariant");
  // initialization complete

  // write order is important because an individual write step
  // might tag an artifact to be written in a subsequent step
  write_klass_constants(writer, leakp_writer);
  if (_artifacts->has_klass_entries()) {
    write_class_loader_constants(writer, leakp_writer);
    write_method_constants(writer, leakp_writer);
    write_symbol_constants(writer, leakp_writer);
    clear_artifacts(_artifacts, class_unload);
  }
}

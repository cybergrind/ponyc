#include "symbols.h"
#include "../codegen/gentype.h"
#include "../../libponyrt/ds/hash.h"
#include "../../libponyrt/mem/pool.h"

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable:4003)
#  pragma warning(disable:4244)
#  pragma warning(disable:4800)
#  pragma warning(disable:4267)
#  pragma warning(disable:4291)
#  pragma warning(disable:4624) //TODO: CHECK
#endif

#include <llvm/IR/Module.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Support/Path.h>
#include <llvm/IR/Metadata.h>
#include <llvm/Config/llvm-config.h>

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

using namespace llvm;
using namespace llvm::dwarf;

typedef struct debug_sym_t debug_sym_t;
typedef struct debug_frame_t debug_frame_t;

struct debug_sym_t
{
  const char* name;

#if PONY_LLVM >= 307
  DIType* type;
  DIType* qualified;
  DIType* actual;
  DICompositeType* prelim;
#else
  DIType type;
  DIType qualified;
  DIType actual;
  DICompositeType prelim;
#endif
};

struct debug_frame_t
{
  const char* type_name;

  size_t size;
  std::vector<llvm::Metadata *> members;

#if PONY_LLVM >= 307
  DIScope* scope;
#else
  DIDescriptor scope;
#endif
  DebugLoc location;

  debug_frame_t* prev;
};

static size_t symbol_hash(debug_sym_t* symbol);
static bool symbol_cmp(debug_sym_t* a, debug_sym_t* b);
static void symbol_free(debug_sym_t* symbol);

DECLARE_HASHMAP(symbolmap, debug_sym_t);
DEFINE_HASHMAP(symbolmap, debug_sym_t, symbol_hash, symbol_cmp,
  pool_alloc_size, pool_free_size, symbol_free);

struct symbols_t
{
  symbolmap_t map;

  DIBuilder* builder;
  IRBuilderBase* ir;
#if PONY_LLVM >= 307
  DICompileUnit* unit;
#else
  DICompileUnit unit;
#endif

  bool release;

  debug_frame_t* frame;
};

static size_t symbol_hash(debug_sym_t* symbol)
{
  return hash_ptr(symbol->name);
}

static bool symbol_cmp(debug_sym_t* a, debug_sym_t* b)
{
  return a->name == b->name;
}

static void symbol_free(debug_sym_t* symbol)
{
  POOL_FREE(debug_sym_t, symbol);
}

static debug_sym_t* get_entry(symbols_t* symbols, const char* name)
{
  debug_sym_t key;
  key.name = name;

  debug_sym_t* value = symbolmap_get(&symbols->map, &key);

  if(value == NULL)
  {
    value = POOL_ALLOC(debug_sym_t);
    value->name = key.name;
#if PONY_LLVM < 307
    value->type = DIType();
    value->qualified = DIType();
    value->prelim = DICompositeType();
    value->actual = DIType();
#endif

    symbolmap_put(&symbols->map, value);
  }

  return value;
}

#if PONY_LLVM >= 307
static DIFile*
#else
static DIFile
#endif
get_file(symbols_t* symbols, const char* fullpath)
{
  StringRef name = sys::path::filename(fullpath);
  StringRef path = sys::path::parent_path(fullpath);

  return symbols->builder->createFile(name, path);
}

void symbols_init(symbols_t** symbols, LLVMBuilderRef builder,
  LLVMModuleRef module, bool optimised)
{
  symbols_t* s = *symbols = POOL_ALLOC(symbols_t);
  memset(s, 0, sizeof(symbols_t));

  symbolmap_init(&s->map, 0);

  Module* m = unwrap(module);

  unsigned version = llvm::DEBUG_METADATA_VERSION;

  m->addModuleFlag(llvm::Module::Warning, "Dwarf Version", version);
  m->addModuleFlag(llvm::Module::Error, "Debug Info Version", version);

  s->builder = new DIBuilder(*m);
  s->ir = unwrap(builder);
  s->release = optimised;
}

void symbols_push_frame(symbols_t* symbols, gentype_t* g)
{
  debug_frame_t* frame = POOL_ALLOC(debug_frame_t);
  memset(frame, 0, sizeof(debug_frame_t));

  if(g != NULL)
  {
    frame->type_name = g->type_name;
    frame->size = g->field_count;
  }

  frame->prev = symbols->frame;
  symbols->frame = frame;
}

void symbols_pop_frame(symbols_t* symbols)
{
  debug_frame_t* frame = symbols->frame;
  symbols->frame = frame->prev;
  POOL_FREE(debug_frame_t, frame);
}

void symbols_package(symbols_t* symbols, const char* path, const char* name)
{
  symbols->unit = symbols->builder->createCompileUnit(DW_LANG_Pony, name, path,
    DW_TAG_Producer, symbols->release, StringRef(), 0, StringRef(),
    llvm::DIBuilder::FullDebug, true);
}

void symbols_basic(symbols_t* symbols, dwarf_meta_t* meta)
{
  debug_sym_t* d = get_entry(symbols, meta->name);

  uint16_t tag = dwarf::DW_ATE_unsigned;

  switch(meta->flags)
  {
    case DWARF_SIGNED:
      tag = dwarf::DW_ATE_signed;
      break;
    case DWARF_FLOAT:
      tag = dwarf::DW_ATE_float;
      break;
    case DWARF_BOOLEAN:
      tag = dwarf::DW_ATE_boolean;
      break;
    default: {};
  }

  d->type = symbols->builder->createBasicType(meta->name, meta->size,
    meta->align, tag);
  d->actual = d->type;

  // Eventually, basic builtin types may be used as const, e.g. let field or
  // local, method/behaviour parameter.
  d->qualified = symbols->builder->createQualifiedType(DW_TAG_const_type,
    d->type);
}

void symbols_pointer(symbols_t* symbols, dwarf_meta_t* meta)
{
  debug_sym_t* pointer = get_entry(symbols, meta->name);
  debug_sym_t* typearg = get_entry(symbols, meta->typearg);

  pointer->type = symbols->builder->createPointerType(typearg->type,
    meta->size, meta->align);

  pointer->actual = pointer->type;

  pointer->qualified = symbols->builder->createQualifiedType(DW_TAG_const_type,
    pointer->type);
}

void symbols_trait(symbols_t* symbols, dwarf_meta_t* meta)
{
  debug_sym_t* d = get_entry(symbols, meta->name);

#if PONY_LLVM >= 307
  DIFile* file = get_file(symbols, meta->file);
#else
  DIFile file = get_file(symbols, meta->file);
#endif

#if PONY_LLVM >= 307
  DICompositeType* composite = symbols->builder->createClassType(symbols->unit,
    meta->name, file, (int)meta->line, meta->size, meta->align, meta->offset,
    0, NULL, DINodeArray());
#else
  DICompositeType composite = symbols->builder->createClassType(symbols->unit,
    meta->name, file, (int)meta->line, meta->size, meta->align, meta->offset,
    0, DIType(), DIArray());
#endif

  d->type = symbols->builder->createPointerType(composite, meta->size,
    meta->align);

  d->actual = d->type;

  d->qualified = symbols->builder->createQualifiedType(DW_TAG_const_type,
    d->type);
}

void symbols_unspecified(symbols_t* symbols, const char* name)
{
  debug_sym_t* d = get_entry(symbols, name);

#if PONY_LLVM >= 307
  DICompositeType* type = symbols->builder->createClassType(symbols->unit,
    name, NULL, 0, 0, 0, 0, 0, NULL, DINodeArray());
#else
  DICompositeType type = symbols->builder->createClassType(symbols->unit,
    name, DIFile(), 0, 0, 0, 0, 0, DIDerivedType(), DIArray());
#endif

  d->type = symbols->builder->createPointerType(type, 0, 0);

  d->actual = d->type;

  d->qualified = symbols->builder->createQualifiedType(DW_TAG_const_type,
    d->type);
}

void symbols_declare(symbols_t* symbols, dwarf_meta_t* meta)
{
  debug_sym_t* d = get_entry(symbols, meta->name);

#if PONY_LLVM >= 307
  DIFile* file = get_file(symbols, meta->file);
#else
  DIFile file = get_file(symbols, meta->file);
#endif
  uint16_t tag = DW_TAG_class_type;
  uint16_t qualifier = DW_TAG_const_type;

  if(meta->flags & DWARF_TUPLE)
    tag = DW_TAG_structure_type;

#if PONY_LLVM >= 307
  d->prelim = symbols->builder->createReplaceableCompositeType(tag,
    meta->name, symbols->unit, file, (int)meta->line);
#else
  d->prelim = symbols->builder->createReplaceableForwardDecl(tag,
    meta->name, symbols->unit, file, (int)meta->line);
#endif

  if(meta->flags & DWARF_TUPLE)
  {
    // The actual use type is the structure itself.
    d->type = d->prelim;
    d->qualified = symbols->builder->createQualifiedType(qualifier, d->type);
  } else {
    // The use type is a pointer to the structure.
    d->type = symbols->builder->createPointerType(d->prelim,
      meta->size, meta->align);

    // A let field or method parameter is equivalent to a
    // C <type>* const <identifier>.
    d->qualified = symbols->builder->createQualifiedType(qualifier,
      d->type);
  }
}

void symbols_field(symbols_t* symbols, dwarf_meta_t* meta)
{
  debug_sym_t* d = get_entry(symbols, meta->typearg);
  debug_frame_t* frame = symbols->frame;

  unsigned visibility = DW_ACCESS_public;

  if((meta->flags & DWARF_PRIVATE) != 0)
    visibility = DW_ACCESS_private;

#if PONY_LLVM >= 307
  DIType* use_type = d->type;
#else
  DIType use_type = d->type;
#endif

  if(meta->flags & DWARF_CONSTANT)
    use_type = d->qualified;

#if PONY_LLVM >= 307
  DIFile* file = get_file(symbols, meta->file);

  DIDerivedType* member = symbols->builder->createMemberType(symbols->unit,
    meta->name, file, (int)meta->line, meta->size, meta->align, meta->offset,
    visibility, use_type);
#else
  DIFile file = get_file(symbols, meta->file);

  DIDerivedType member = symbols->builder->createMemberType(symbols->unit,
    meta->name, file, (int)meta->line, meta->size, meta->align, meta->offset,
    visibility, use_type);
#endif

  frame->members.push_back(member);
}

void symbols_method(symbols_t* symbols, dwarf_meta_t* meta, LLVMValueRef ir)
{
  // Emit debug info for the subroutine type.
  std::vector<Metadata*> params;

  // The return type is not const, so don't use the qualified type.
  debug_sym_t* current = get_entry(symbols, meta->params[0]);
  params.push_back(current->type);

  for(size_t i = 1; i < meta->size; i++)
  {
    current = get_entry(symbols, meta->params[i]);
    params.push_back(current->qualified);
  }

#if PONY_LLVM >= 307
  DIFile* file = get_file(symbols, meta->file);
#else
  DIFile file = get_file(symbols, meta->file);
#endif

#if PONY_LLVM >= 307
  DISubroutineType* type = symbols->builder->createSubroutineType(file,
    symbols->builder->getOrCreateTypeArray(params));
#else
  DICompositeType type = symbols->builder->createSubroutineType(file,
    symbols->builder->getOrCreateTypeArray(params));
#endif

  Function* f = dyn_cast_or_null<Function>(unwrap(ir));
  debug_sym_t* d = get_entry(symbols, symbols->frame->type_name);

#if PONY_LLVM >= 307
  symbols->frame->scope = symbols->builder->createMethod(d->actual,
    meta->name, meta->mangled, file, (int)meta->line, type, false, true,
    0, 0, NULL, 0, symbols->release, f);
#else
  symbols->frame->scope = symbols->builder->createMethod(d->actual,
    meta->name, meta->mangled, file, (int)meta->line, type, false, true,
    0, 0, DIType(), 0, symbols->release, f);
#endif
}

void symbols_composite(symbols_t* symbols, dwarf_meta_t* meta)
{
  // The composite was previously forward declared, and a preliminary
  // debug symbol exists.
#if PONY_LLVM >= 307
  DIFile* file = get_file(symbols, meta->file);
  DINodeArray fields = symbols->builder->getOrCreateArray(symbols->frame->members);
#else
  DIFile file = get_file(symbols, meta->file);
  DIArray fields = symbols->builder->getOrCreateArray(symbols->frame->members);
#endif
  
  debug_sym_t* d = get_entry(symbols, meta->name);

  if(meta->flags & DWARF_TUPLE)
  {
#if PONY_LLVM >= 307
    d->actual = symbols->builder->createStructType(symbols->unit,
      meta->name, file, (int)meta->line, meta->size, meta->align, 0, NULL,
      fields);
#else
    d->actual = symbols->builder->createStructType(symbols->unit,
      meta->name, file, (int)meta->line, meta->size, meta->align, 0, DIType(),
      fields);
#endif

    d->type = d->actual;
  } else {
#if PONY_LLVM >= 307
    d->actual = symbols->builder->createClassType(symbols->unit, meta->name,
      file, (int)meta->line, meta->size, meta->align, 0, 0, NULL, fields);
#else    
    d->actual = symbols->builder->createClassType(symbols->unit, meta->name,
      file, (int)meta->line, meta->size, meta->align, 0, 0, DIType(), fields);
#endif
  }

#if PONY_LLVM >= 307
  d->prelim->replaceAllUsesWith(d->actual);
#else
  d->prelim.replaceAllUsesWith(d->actual);
#endif
}

void symbols_lexicalscope(symbols_t* symbols, dwarf_meta_t* meta)
{
#if PONY_LLVM >= 307
  DIScope* parent = symbols->frame->prev->scope;
  DIFile* file = get_file(symbols, meta->file);
#else
  DIDescriptor parent = symbols->frame->prev->scope;
  DIFile file = get_file(symbols, meta->file);
#endif

  assert((MDNode*)parent != NULL);

  symbols->frame->scope = symbols->builder->createLexicalBlock(parent,
    file, (unsigned)meta->line, (unsigned)meta->pos);
}

void symbols_local(symbols_t* symbols, dwarf_meta_t* meta, bool is_arg)
{
  unsigned tag = DW_TAG_auto_variable;
  unsigned index = 0;

  debug_sym_t* d = get_entry(symbols, meta->mangled);
  debug_frame_t* frame = symbols->frame;

#if PONY_LLVM >= 307
  DIType* type = d->type;
  DIFile* file = get_file(symbols, meta->file);
#else
  DIType type = d->type;
  DIFile file = get_file(symbols, meta->file);
#endif

  if(is_arg)
  {
    tag = DW_TAG_arg_variable;
    index = (unsigned)meta->offset;
  }

  if(meta->flags & DWARF_CONSTANT)
    type = d->qualified;

  if(meta->flags & DWARF_ARTIFICIAL)
    type = symbols->builder->createArtificialType(type);

#if PONY_LLVM >= 307
  DILocalVariable* info = symbols->builder->createLocalVariable(tag, frame->scope,
    meta->name, file, (unsigned)meta->line, type, true, 0, index);

  DIExpression* complex = symbols->builder->createExpression();
#else
  DIVariable info = symbols->builder->createLocalVariable(tag, frame->scope,
    meta->name, file, (unsigned)meta->line, type, true, 0, index);

  DIExpression complex = symbols->builder->createExpression();
#endif

  Value* ref = unwrap(meta->storage);
  Instruction* intrinsic;
  DebugLoc location = DebugLoc::get((unsigned)meta->line,
    (unsigned)meta->pos, frame->scope);

  if(meta->inst != NULL)
  {
    Instruction* before = dyn_cast_or_null<Instruction>(unwrap(meta->inst));
#if PONY_LLVM >= 307
    intrinsic = symbols->builder->insertDeclare(ref, info, complex, location,
      before);
#else
    intrinsic = symbols->builder->insertDeclare(ref, info, complex, before);
#endif
  } else {
    BasicBlock* end = dyn_cast_or_null<BasicBlock>(unwrap(meta->entry));
#if PONY_LLVM >= 307
    intrinsic = symbols->builder->insertDeclare(ref, info, complex, location, end);
#else
    intrinsic = symbols->builder->insertDeclare(ref, info, complex, end);
#endif
  }

#if PONY_LLVM < 307
  intrinsic->setDebugLoc(DebugLoc::get((unsigned)meta->line,
    (unsigned)meta->pos, frame->scope));
#else
  (void)intrinsic;
#endif
}

void symbols_location(symbols_t* symbols, size_t line, size_t pos)
{
  DebugLoc loc =  DebugLoc::get((unsigned)line, (unsigned)pos,
    symbols->frame->scope);

  symbols->frame->location = loc;
  symbols->ir->SetCurrentDebugLocation(loc);
}

void symbols_reset(symbols_t* symbols, bool disable)
{
  if(disable && (symbols->frame != NULL))
  {
    symbols->frame->location = DebugLoc::get(0, 0, NULL);
    symbols->ir->SetCurrentDebugLocation(symbols->frame->location);
  }
  else if(symbols->frame)
  {
    symbols->ir->SetCurrentDebugLocation(symbols->frame->location);
  }
}

void symbols_finalise(symbols_t* symbols)
{
  symbols->builder->finalize();
  delete symbols->builder;

  symbolmap_destroy(&symbols->map);
  POOL_FREE(symbols_t, symbols);
}

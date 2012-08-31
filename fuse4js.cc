#include <node.h>
#include <node_buffer.h>
#include <v8.h>

#define FUSE_USE_VERSION 26

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#ifdef HAVE_SETXATTR
#include <sys/xattr.h>
#endif

#include <fuse.h>
#include <semaphore.h>
#include <string>

using namespace v8;

// ---------------------------------------------------------------------------

static struct {
  uv_async_t async;
  sem_t sem;
  pthread_t fuse_thread;
  char root[256];
  Persistent<Object> handlers;
  Persistent<Object> nodeBuffer;  
} f4js;

enum fuseop_t {  
  OP_EXIT = 0,
  OP_GETATTR = 1,
  OP_READDIR = 2,
  OP_OPEN = 3,
  OP_READ = 4,
  OP_WRITE = 5,
  OP_CREATE = 6,
  OP_UNLINK = 7,
  OP_RENAME = 8,
  OP_MKDIR = 9,
  OP_RMDIR = 10,
  OP_INIT = 11,
  OP_DESTROY = 12
};

const char* fuseop_names[] = {
    "exit",
    "getattr",
    "readdir",
    "open",
    "read",
    "write",
    "create",
    "unlink",
    "rename",
    "mkdir",
    "rmdir",
    "init",
    "destroy"
};

static struct {
  enum fuseop_t op;
  const char *in_path;
  union {
    struct {
      struct stat *stbuf;
    } getattr;
    struct {
      void *buf;
      fuse_fill_dir_t filler;
    } readdir;
    struct {
      off_t offset;
      size_t len;
      char *dstBuf;
      const char *srcBuf; 
    } rw;
    struct {
      const char *dst;
    } rename;
  } u;
  int retval;
} f4js_cmd;

// ---------------------------------------------------------------------------

static int f4js_rpc(enum fuseop_t op, const char *path)
{
  f4js_cmd.op = op;
  f4js_cmd.in_path = path;
  uv_async_send(&f4js.async);
  sem_wait(&f4js.sem);
  return f4js_cmd.retval;  
}

// ---------------------------------------------------------------------------

static int f4js_getattr(const char *path, struct stat *stbuf)
{
  f4js_cmd.u.getattr.stbuf = stbuf;
  return f4js_rpc(OP_GETATTR, path);
}

// ---------------------------------------------------------------------------

static int f4js_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
		         off_t offset, struct fuse_file_info *fi)
{
  f4js_cmd.u.readdir.buf = buf;
  f4js_cmd.u.readdir.filler = filler;
  return f4js_rpc(OP_READDIR, path);
}

// ---------------------------------------------------------------------------

int f4js_open(const char *path, struct fuse_file_info *)
{
  return f4js_rpc(OP_OPEN, path);
}

// ---------------------------------------------------------------------------

int f4js_read (const char *path,
               char *buf,
               size_t len,
               off_t offset,
               struct fuse_file_info *)
{
  f4js_cmd.u.rw.offset = offset;
  f4js_cmd.u.rw.len = len;
  f4js_cmd.u.rw.dstBuf = buf;
  return f4js_rpc(OP_READ, path);
}

// ---------------------------------------------------------------------------

int f4js_write (const char *path,
                const char *buf,
                size_t len,
                off_t offset,
                struct fuse_file_info *)
{
  f4js_cmd.u.rw.offset = offset;
  f4js_cmd.u.rw.len = len;
  f4js_cmd.u.rw.srcBuf = buf;
  return f4js_rpc(OP_WRITE, path);
}

// ---------------------------------------------------------------------------

int f4js_create (const char *path,
                 mode_t mode,
                 struct fuse_file_info *)
{
  return f4js_rpc(OP_CREATE, path);
}

// ---------------------------------------------------------------------------

int f4js_utimens (const char *,
                  const struct timespec tv[2])
{
  return 0; // stub out for now to make "touch" command succeed
}

// ---------------------------------------------------------------------------

int f4js_unlink (const char *path)
{
  return f4js_rpc(OP_UNLINK, path);
}

// ---------------------------------------------------------------------------

int f4js_rename (const char *src, const char *dst)
{
  f4js_cmd.u.rename.dst = dst;
  return f4js_rpc(OP_RENAME, src);
}

// ---------------------------------------------------------------------------

int f4js_mkdir (const char *path, mode_t mode)
{
  // TODO: pass 'mode'
  return f4js_rpc(OP_MKDIR, path);
}

// ---------------------------------------------------------------------------

int f4js_rmdir (const char *path)
{
  return f4js_rpc(OP_RMDIR, path);
}

// ---------------------------------------------------------------------------


void* f4js_init(struct fuse_conn_info *conn)
{
  // We currently always return NULL
  return (void*)f4js_rpc(OP_INIT, "");
}

// ---------------------------------------------------------------------------

void f4js_destroy (void *data)
{
  // We currently ignore the data pointer, which init() always sets to NULL
  f4js_rpc(OP_DESTROY, "");
}

// ---------------------------------------------------------------------------

void *fuse_thread(void *)
{
  struct fuse_operations ops = { 0 };
  ops.getattr = f4js_getattr;
  ops.readdir = f4js_readdir;
  ops.open = f4js_open;
  ops.read = f4js_read;
  ops.write = f4js_write;
  ops.create = f4js_create;
  ops.utimens = f4js_utimens;
  ops.unlink = f4js_unlink;
  ops.rename = f4js_rename;
  ops.mkdir = f4js_mkdir;
  ops.rmdir = f4js_rmdir;
  ops.init = f4js_init;
  ops.destroy = f4js_destroy;
  char *argv[] = { (char*)"dummy", (char*)"-s", (char*)"-d", f4js.root };
  fuse_main(4, argv, &ops, NULL);
  f4js_cmd.in_path = ""; // Ugly. To make DispatchOp() happy.
  f4js_cmd.op = OP_EXIT;
  uv_async_send(&f4js.async);
  return NULL;
}

// ---------------------------------------------------------------------------

Handle<Value> GetAttrCompletion(const Arguments& args)
{
  HandleScope scope;
  if (args.Length() >= 1 && args[0]->IsNumber()) {
    Local<Number> retval = Local<Number>::Cast(args[0]);
    f4js_cmd.retval = (int)retval->Value();
    if (f4js_cmd.retval == 0 && args.Length() >= 2 && args[1]->IsObject()) {
      memset(f4js_cmd.u.getattr.stbuf, 0, sizeof(*f4js_cmd.u.getattr.stbuf));
      Handle<Object> stat = Handle<Object>::Cast(args[1]);
      
      Local<Value> prop = stat->Get(String::NewSymbol("st_size"));
      if (!prop->IsUndefined() && prop->IsNumber()) {
        Local<Number> num = Local<Number>::Cast(prop);
        f4js_cmd.u.getattr.stbuf->st_size = (off_t)num->Value();
      }
      
      prop = stat->Get(String::NewSymbol("st_mode"));
      if (!prop->IsUndefined() && prop->IsNumber()) {
        Local<Number> num = Local<Number>::Cast(prop);
        f4js_cmd.u.getattr.stbuf->st_mode = (mode_t)num->Value();
      }
    }
  }
  sem_post(&f4js.sem);  
  return scope.Close(Undefined());    
}

// ---------------------------------------------------------------------------

Handle<Value> ReadDirCompletion(const Arguments& args)
{
  HandleScope scope;
  if (args.Length() >= 1 && args[0]->IsNumber()) {
    Local<Number> retval = Local<Number>::Cast(args[0]);
    f4js_cmd.retval = (int)retval->Value();
    if (f4js_cmd.retval == 0 && args.Length() >= 2 && args[1]->IsArray()) {
      Handle<Array> ar = Handle<Array>::Cast(args[1]);
      for (uint32_t i = 0; i < ar->Length(); i++) {
        Local<Value> el = ar->Get(i);
        if (!el->IsUndefined() && el->IsString()) {
          Local<String> name = Local<String>::Cast(el);
          String::AsciiValue av(name);          
          struct stat st;
          memset(&st, 0, sizeof(st)); // structure not used. Zero everything.
          if (f4js_cmd.u.readdir.filler(f4js_cmd.u.readdir.buf, *av, &st, 0))
            break;            
        }
      }
    }
  }
  sem_post(&f4js.sem);  
  return scope.Close(Undefined());    
}

// ---------------------------------------------------------------------------

Handle<Value> GenericCompletion(const Arguments& args)
{
  HandleScope scope;
  if (args.Length() >= 1 && args[0]->IsNumber()) {
    Local<Number> retval = Local<Number>::Cast(args[0]);
    f4js_cmd.retval = (int)retval->Value();    
  }
  sem_post(&f4js.sem);  
  return scope.Close(Undefined());    
}

// ---------------------------------------------------------------------------

Handle<Value> ReadCompletion(const Arguments& args)
{
  HandleScope scope;
  if (args.Length() >= 1 && args[0]->IsNumber()) {
    Local<Number> retval = Local<Number>::Cast(args[0]);
    f4js_cmd.retval = (int)retval->Value();
    if (f4js_cmd.retval >= 0) {
      char *buffer_data = node::Buffer::Data(f4js.nodeBuffer);
      if ((size_t)f4js_cmd.retval > f4js_cmd.u.rw.len) {
        f4js_cmd.retval = f4js_cmd.u.rw.len;
      }
      memcpy(f4js_cmd.u.rw.dstBuf, buffer_data, f4js_cmd.retval);
    }
  }
  f4js.nodeBuffer.Dispose();
  sem_post(&f4js.sem);  
  return scope.Close(Undefined());    
}

// ---------------------------------------------------------------------------

Handle<Value> WriteCompletion(const Arguments& args)
{
  HandleScope scope;
  if (args.Length() >= 1 && args[0]->IsNumber()) {
    Local<Number> retval = Local<Number>::Cast(args[0]);
    f4js_cmd.retval = (int)retval->Value();
  }
  f4js.nodeBuffer.Dispose();
  sem_post(&f4js.sem);  
  return scope.Close(Undefined());    
}

// ---------------------------------------------------------------------------

// Called from the main thread.
static void DispatchOp(uv_async_t* handle, int status)
{
  HandleScope scope;
  std::string symName(fuseop_names[f4js_cmd.op]);
  Local<FunctionTemplate> tpl = FunctionTemplate::New(GenericCompletion); // default
  f4js_cmd.retval = -EPERM;
  int argc = 0;
  Handle<Value> argv[5];  
  Local<String> path = String::New(f4js_cmd.in_path);  
  argv[argc++] = path;
  node::Buffer* buffer = NULL; // used for read/write operations
  
  switch (f4js_cmd.op) {
  case OP_EXIT:
    pthread_join(f4js.fuse_thread, NULL);
    uv_unref((uv_handle_t*) &f4js.async);
    return;
    
  case OP_INIT:
  case OP_DESTROY:
    f4js_cmd.retval = 0; // Will be used as the return value of OP_INIT.
    --argc;  // Ugly. Remove the first argument (path) because it's not needed.
    break;
    
  case OP_GETATTR:
    tpl = FunctionTemplate::New(GetAttrCompletion);
    break;
  
  case OP_READDIR:
    tpl = FunctionTemplate::New(ReadDirCompletion);
    break;
  
  case OP_RENAME:
    argv[argc++] = String::New(f4js_cmd.u.rename.dst);
    break;

  case OP_READ:
    tpl = FunctionTemplate::New(ReadCompletion);
    buffer = node::Buffer::New(f4js_cmd.u.rw.len);
    break;
    
  case OP_WRITE:
    tpl = FunctionTemplate::New(WriteCompletion);   
    buffer = node::Buffer::New((char*)f4js_cmd.u.rw.srcBuf, f4js_cmd.u.rw.len);
    break;
    
  default:
    break;
  }
  
  // Additional args for read/write operations
  if (buffer) { 
    // FIXME: 64-bit off_t cannot always fit in a JS number 
    argv[argc++] = Number::New((double)f4js_cmd.u.rw.offset);  
    argv[argc++] = Number::New((double)f4js_cmd.u.rw.len);
    f4js.nodeBuffer = Persistent<Object>::New(buffer->handle_);   
    argv[argc++] = f4js.nodeBuffer;
  }
  
  Local<Function> handler = Local<Function>::Cast(f4js.handlers->Get(String::NewSymbol(symName.c_str())));
  if (handler->IsUndefined()) {
    sem_post(&f4js.sem);
    return;
  }
  
  Local<Function> cb = tpl->GetFunction();
  std::string cbName = symName + "Completion";
  cb->SetName(String::NewSymbol(cbName.c_str()));
  argv[argc++] = cb;
  handler->Call(Context::GetCurrent()->Global(), argc, argv);  
}

// ---------------------------------------------------------------------------

Handle<Value> Start(const Arguments& args)
{
  HandleScope scope;
  if (args.Length() < 2) {
    ThrowException(Exception::TypeError(String::New("Wrong number of arguments")));
    return scope.Close(Undefined());
  }

  if (!args[0]->IsString() || !args[1]->IsObject()) {
    ThrowException(Exception::TypeError(String::New("Wrong argument types")));
    return scope.Close(Undefined());
  }

  String::AsciiValue av(args[0]);
  char *root = *av;
  if (root == NULL) {
    ThrowException(Exception::TypeError(String::New("Path is incorrect")));
    return scope.Close(Undefined());
  }
  strncpy(f4js.root, root, sizeof(f4js.root));
  f4js.handlers = Persistent<Object>::New(Local<Object>::Cast(args[1]));

  sem_init(&f4js.sem, 0, 0);
  uv_async_init(uv_default_loop(), &f4js.async, DispatchOp);

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_create(&f4js.fuse_thread, &attr, fuse_thread, NULL);
  return scope.Close(String::New("dummy"));
}

// ---------------------------------------------------------------------------

void init(Handle<Object> target)
{
  target->Set(String::NewSymbol("start"), FunctionTemplate::New(Start)->GetFunction());
}

// ---------------------------------------------------------------------------

NODE_MODULE(fuse4js, init)

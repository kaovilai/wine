/*
 * IDL Compiler
 *
 * Copyright 2002 Ove Kaaven
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <signal.h>

#include "widl.h"
#include "utils.h"
#include "parser.h"
#include "header.h"

/* FIXME: support generation of stubless proxies */

static void write_stubdesc(void)
{
  fprintf(proxy, "const MIDL_STUB_DESC Object_StubDesc = {\n");
  fprintf(proxy, "    0,\n");
  fprintf(proxy, "    NdrOleAllocate,\n");
  fprintf(proxy, "    NdrOleFree,\n");
  fprintf(proxy, "    {0}, 0, 0, 0, 0,\n");
  fprintf(proxy, "    0 /* __MIDL_TypeFormatString.Format */\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");
}

static void init_proxy(void)
{
  if (proxy) return;
  proxy = fopen(proxy_name, "w");
  fprintf(proxy, "/*** Autogenerated by WIDL %s - Do not edit ***/\n", WIDL_FULLVERSION);
  fprintf(proxy, "#include \"rpcproxy.h\"\n");
  fprintf(proxy, "#include \"%s\"\n", header_name);
  fprintf(proxy, "\n");
  write_stubdesc();
}

static void gen_proxy(type_t *iface, func_t *cur, int idx)
{
  var_t *def = cur->def;
  int has_ret = !is_void(def->type, def);

  write_type(proxy, def->type, def, def->tname);
  fprintf(proxy, " CALLBACK %s_", iface->name);
  write_name(proxy, def);
  fprintf(proxy, "_Proxy(\n");
  write_method_args(proxy, cur->args, iface->name);
  fprintf(proxy, ")\n");
  fprintf(proxy, "{\n");
  /* local variables */
  if (has_ret) {
    fprintf(proxy, "    ");
    write_type(proxy, def->type, def, def->tname);
    fprintf(proxy, " _Ret;\n");
  }
  fprintf(proxy, "    RPC_MESSAGE _Msg;\n");
  fprintf(proxy, "    MIDL_STUB_MESSAGE _StubMsg;\n");
  fprintf(proxy, "\n");

  /* FIXME: trace */
  /* FIXME: clear output vars? */

  fprintf(proxy, "    NdrProxyInitialize(This, &_Msg, &_StubMsg, &Object_StubDesc, %d);\n", idx);

  /* FIXME: size buffer */

  fprintf(proxy, "    NdrProxyGetBuffer(This, &_StubMsg);\n");

  /* FIXME: marshall */

  fprintf(proxy, "    NdrProxySendReceive(This, &_StubMsg);\n");

  /* FIXME: unmarshall */

  fprintf(proxy, "    NdrProxyFreeBuffer(This, &_StubMsg);\n");

  if (has_ret) {
    fprintf(proxy, "    return _Ret;\n");
  }
  fprintf(proxy, "}\n");
  fprintf(proxy, "\n");
}

static void gen_stub(type_t *iface, func_t *cur, char *cas)
{
  var_t *def = cur->def;
  var_t *arg;
  int has_ret = !is_void(def->type, def);

  fprintf(proxy, "void __RPC_STUB %s_", iface->name);
  write_name(proxy, def);
  fprintf(proxy, "_Stub(\n");
  fprintf(proxy, "    IRpcStubBuffer* This,\n");
  fprintf(proxy, "    IRpcChannelBuffer* pRpcChannelBuffer,\n");
  fprintf(proxy, "    PRPC_MESSAGE pRpcMessage,\n");
  fprintf(proxy, "    DWORD* pdwStubPhase)\n");
  fprintf(proxy, "{\n");
  /* local variables */
  if (has_ret) {
    fprintf(proxy, "    ");
    write_type(proxy, def->type, def, def->tname);
    fprintf(proxy, " _Ret;\n");
  }
  fprintf(proxy, "    %s* _This = (%s*)((CStdStubBuffer*)This)->pvServerObject;\n", iface->name, iface->name);
  fprintf(proxy, "    MIDL_STUB_MESSAGE _StubMsg;\n");
  arg = cur->args;
  while (arg) {
    fprintf(proxy, "    ");
    write_type(proxy, arg->type, arg, arg->tname);
    fprintf(proxy, " ");
    write_name(proxy, arg);
    fprintf(proxy, ";\n");
    arg = NEXT_LINK(arg);
  }
  fprintf(proxy, "\n");

  /* FIXME: trace */
  /* FIXME: clear output vars? */

  fprintf(proxy, "    NdrStubInitialize(pRpcMessage, &_StubMsg, &Object_StubDesc, pRpcChannelBuffer);\n");

  /* FIXME: unmarshall */

  fprintf(proxy, "    *pdwStubPhase = STUB_CALL_SERVER;\n");
  fprintf(proxy, "    ");
  if (has_ret) fprintf(proxy, "_Ret = ");
  fprintf(proxy, "%s_", iface->name);
  if (cas) fprintf(proxy, "%s_Stub", cas);
  else write_name(proxy, def);
  fprintf(proxy, "(_This");
  arg = cur->args;
  if (arg) {
    while (NEXT_LINK(arg)) arg = NEXT_LINK(arg);
    while (arg) {
      fprintf(proxy, ", ");
      write_name(proxy, arg);
      arg = PREV_LINK(arg);
    }
  }
  fprintf(proxy, ");\n");
  fprintf(proxy, "    *pdwStubPhase = STUB_MARSHAL;\n");

  /* FIXME: size buffer */

  fprintf(proxy, "    NdrStubGetBuffer(This, pRpcChannelBuffer, &_StubMsg);\n");

  /* FIXME: marshall */

  fprintf(proxy, "}\n");
  fprintf(proxy, "\n");
}

static int write_proxy_methods(type_t *iface)
{
  func_t *cur = iface->funcs;
  int i = 0;
  while (NEXT_LINK(cur)) cur = NEXT_LINK(cur);

  if (iface->ref) i = write_proxy_methods(iface->ref);
  while (cur) {
    var_t *def = cur->def;
    if (!is_callas(def->attrs)) {
      if (i) fprintf(proxy, ",\n     ");
      fprintf(proxy, "%s_", iface->name);
      write_name(proxy, def);
      fprintf(proxy, "_Proxy");
      i++;
    }
    cur = PREV_LINK(cur);
  }
  return i;
}

static int write_stub_methods(type_t *iface)
{
  func_t *cur = iface->funcs;
  int i = 0;
  while (NEXT_LINK(cur)) cur = NEXT_LINK(cur);

  if (iface->ref) i = write_stub_methods(iface->ref);
  else return i; /* skip IUnknown */
  while (cur) {
    var_t *def = cur->def;
    if (!is_local(def->attrs)) {
      if (i) fprintf(proxy, ",\n");
      fprintf(proxy, "    %s_", iface->name);
      write_name(proxy, def);
      fprintf(proxy, "_Stub");
      i++;
    }
    cur = PREV_LINK(cur);
  }
  return i;
}

typedef struct _if_list if_list;
struct _if_list {
  type_t *iface;
  DECL_LINK(if_list)
};

if_list *if_first;

void write_proxy(type_t *iface)
{
  int midx = -1, stubs;
  func_t *cur = iface->funcs;
  if_list *if_cur;

  if (!cur) {
    yywarning("%s has no methods", iface->name);
    return;
  }

  while (NEXT_LINK(cur)) cur = NEXT_LINK(cur);

  /* FIXME: check for [oleautomation], shouldn't generate proxies/stubs if specified */

  init_proxy();

  if_cur = xmalloc(sizeof(if_list));
  if_cur->iface = iface;
  INIT_LINK(if_cur);
  LINK(if_cur, if_first);
  if_first = if_cur;

  fprintf(proxy, "/*****************************************************************************\n");
  fprintf(proxy, " * %s interface\n", iface->name);
  fprintf(proxy, " */\n");
  while (cur) {
    var_t *def = cur->def;
    if (!is_local(def->attrs)) {
      var_t *cas = is_callas(def->attrs);
      char *cname = cas ? cas->name : NULL;
      int idx = cur->idx;
      if (cname) {
        func_t *m = iface->funcs;
        while (m && strcmp(get_name(m->def), cname))
          m = NEXT_LINK(m);
        idx = m->idx;
      }
      gen_proxy(iface, cur, idx);
      gen_stub(iface, cur, cname);
      if (midx == -1) midx = idx;
      else if (midx != idx) yyerror("method index mismatch in write_proxy");
      midx++;
    }
    cur = PREV_LINK(cur);
  }

  /* proxy vtable */
  fprintf(proxy, "const CINTERFACE_PROXY_VTABLE(%d) %sProxyVtbl = {\n", midx, iface->name);
  fprintf(proxy, "    {&IID_%s},\n", iface->name);
  fprintf(proxy, "    {");
  write_proxy_methods(iface);
  fprintf(proxy, "}\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  /* stub vtable */
  fprintf(proxy, "static const PRPC_STUB_FUNCTION %s_table[] = {\n", iface->name);
  stubs = write_stub_methods(iface);
  fprintf(proxy, "\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");
  fprintf(proxy, "const CInterfaceStubVtbl %sStubVtbl = {\n", iface->name);
  fprintf(proxy, "    {&IID_%s,\n", iface->name);
  fprintf(proxy, "     0,\n");
  fprintf(proxy, "     %d,\n", stubs+3);
  fprintf(proxy, "     &%s_table[-3]},\n", iface->name);
  fprintf(proxy, "    {CStdStubBuffer_METHODS}\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");
}

void finish_proxy(void)
{
  if_list *lcur = if_first;
  if_list *cur;
  char *file_id = "XXX";
  int c;

  if (!lcur) return;
  while (NEXT_LINK(lcur)) lcur = NEXT_LINK(lcur);

  fprintf(proxy, "const CInterfaceProxyVtbl* _%s_ProxyVtblList[] = {\n", file_id);
  cur = lcur;
  while (cur) {
    fprintf(proxy, "    (CInterfaceProxyVtbl*)&%sProxyVtbl,\n", cur->iface->name);
    cur = PREV_LINK(cur);
  }
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "const CInterfaceStubVtbl* _%s_StubVtblList[] = {\n", file_id);
  cur = lcur;
  while (cur) {
    fprintf(proxy, "    (CInterfaceStubVtbl*)&%sStubVtbl,\n", cur->iface->name);
    cur = PREV_LINK(cur);
  }
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "const PCInterfaceName _%s_InterfaceNamesList[] = {\n", file_id);
  cur = lcur;
  while (cur) {
    fprintf(proxy, "    \"%s\",\n", cur->iface->name);
    cur = PREV_LINK(cur);
  }
  fprintf(proxy, "    0\n");
  fprintf(proxy, "};\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "#define _%s_CHECK_IID(n) IID_GENERIC_CHECK_IID(_XXX, pIID, n)\n", file_id);
  fprintf(proxy, "\n");
  fprintf(proxy, "int __stdcall _%s_IID_Lookup(const IID* pIID, int* pIndex)\n", file_id);
  fprintf(proxy, "{\n");
  cur = lcur;
  c = 0;
  while (cur) {
    fprintf(proxy, "    if (!_%s_CHECK_IID(%d)) {\n", file_id, c);
    fprintf(proxy, "        *pIndex = %d\n", c);
    fprintf(proxy, "        return 1;\n");
    fprintf(proxy, "    }\n");
    cur = PREV_LINK(cur);
    c++;
  }
  fprintf(proxy, "    return 0;\n");
  fprintf(proxy, "}\n");
  fprintf(proxy, "\n");

  fprintf(proxy, "const ExtendedProxyFileInfo %s_ProxyFileInfo = {\n", file_id);
  fprintf(proxy, "    (PCInterfaceProxyVtblList*)&_%s_ProxyVtblList,\n", file_id);
  fprintf(proxy, "    (PCInterfaceStubVtblList*)&_%s_StubVtblList,\n", file_id);
  fprintf(proxy, "    (const PCInterfaceName*)&_%s_InterfaceNamesList,\n", file_id);
  fprintf(proxy, "    0,\n");
  fprintf(proxy, "    &_%s_IID_Lookup,\n", file_id);
  fprintf(proxy, "    %d,\n", c);
  fprintf(proxy, "    1\n");
  fprintf(proxy, "};\n");

  fclose(proxy);
}

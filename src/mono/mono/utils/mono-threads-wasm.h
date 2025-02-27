// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.
/**
 * \file
 * Low-level threading for Webassembly
 */

#ifndef __MONO_THREADS_WASM_H__
#define __MONO_THREADS_WASM_H__

#include <glib.h>
#include <mono/utils/mono-threads.h>

#ifdef HOST_WASM

/*
 * declared in mono-threads.h
 *
 * gboolean
 * mono_threads_platform_is_main_thread (void);
 */

gboolean
mono_threads_wasm_is_browser_thread (void);

MonoNativeThreadId
mono_threads_wasm_browser_thread_tid (void);

#ifndef DISABLE_THREADS
/**
 * Runs the given function asynchronously on the main thread.
 * See emscripten/threading.h emscripten_async_run_in_main_runtime_thread
 */
void
mono_threads_wasm_async_run_in_main_thread (void (*func) (void));

/*
 * Variant that takes an argument. Add more variants as needed.
 */
void
mono_threads_wasm_async_run_in_main_thread_vi (void (*func)(gpointer), gpointer user_data);

void
mono_threads_wasm_async_run_in_main_thread_vii (void (*func)(gpointer, gpointer), gpointer user_data1, gpointer user_data2);
#endif /* DISABLE_THREADS */

// Called from register_thread when a pthread attaches to the runtime
void
mono_threads_wasm_on_thread_attached (void);

#endif /* HOST_WASM*/

#endif /* __MONO_THREADS_WASM_H__ */

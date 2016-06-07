// PepperPlugin.cpp : Defines the exported functions for the DLL application.
//
#include "PepperPlugin.h"

#include "stdafx.h"

// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <stdlib.h>

#include "ppapi/c/ppb_image_data.h"
#include "ppapi/cpp/graphics_2d.h"
#include "ppapi/cpp/image_data.h"
#include "ppapi/cpp/input_event.h"
#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/point.h"
#include "ppapi/utility/completion_callback_factory.h"

#include <mono/metadata/mono-config.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/class.h>
#include <mono/metadata/metadata.h>
#include <mono/jit/jit.h>
#include <mono/metadata/debug-helpers.h>

#ifdef WIN32
#undef PostMessage
// Allow 'this' in initializer list
#pragma warning(disable : 4355)
#endif

namespace {
	bool initialised = false;
	MonoDomain* monoDomain = NULL;

	const char* getEnvVar(const char *key)
	{

		char * val = getenv(key);
		return val == NULL ? "" : val;
	}

	void split_namespace_class(const char* strc, std::string &nameSpace, std::string &className)
	{
		if (!strc)
			return;

		auto str = std::string(strc);
		int found = str.find_last_of(".");
		
		if (found == -1)
		{
			className = str.substr(0, str.length());
		}
		else
		{
			nameSpace = str.substr(0, found);
			className = str.substr(found + 1, str.length());
		}
	}

	MonoDomain* load_domain()
	{
		MonoDomain* newDomain = mono_domain_create_appdomain("PepperPlugin App Domain", NULL);
		if (!newDomain) {
			fprintf(stderr, "load_domain: Error creating domain\n");
			return nullptr;
		}

		if (!mono_domain_set(newDomain, false)) {
			fprintf(stderr, "load_domain: Error setting domain\n");
			return nullptr;
		}

		return mono_domain_get();
	}

	void InitMono() {

		if (initialised)
			return;

		printf("Initialising Mono!\n");
		// point to the relevant directories of the Mono installation
		auto mono_root = std::string(getEnvVar("MONO_ROOT"));
		auto mono_lib = mono_root + "/lib";
		auto mono_etc = mono_root + "/etc";
		mono_set_dirs(mono_lib.c_str(), mono_etc.c_str());

		//mono_set_dirs("C:\\Program Files (x86)\\Mono\\lib",
		//	"C:\\Program Files (x86)\\Mono\\etc");

		////// load the default Mono configuration file in 'etc/mono/config'
		mono_config_parse(nullptr);

		MonoDomain* domain = mono_jit_init_version("PepperPlugin Domain", "v4.0.30319");

		initialised = true;

		if (!monoDomain)
		{
			printf("load_domain\n");
			monoDomain = load_domain();
		}
	}

}  // namespace

class PluginInstance : public pp::Instance {
public:
	explicit PluginInstance(PP_Instance instance)
		: pp::Instance(instance) 
	{
		InitMono();
	}

	~PluginInstance() {  }

	virtual bool Init(uint32_t argc, const char* argn[], const char* argv[]) {

		auto className = std::string();
		auto nameSpace = std::string();

		MonoArray *parmsArgN = mono_array_new(monoDomain, mono_get_string_class(), argc);
		MonoArray *parmsArgV = mono_array_new(monoDomain, mono_get_string_class(), argc);

		for (unsigned int x = 0; x < argc; x++)
		{
			mono_array_set(parmsArgN, MonoString*, x, mono_string_new(monoDomain, argn[x]));
			mono_array_set(parmsArgV, MonoString*, x, mono_string_new(monoDomain, argv[x]));

			//printf("argn = %s argv = %s\n", argn[x], argv[x]);
			auto property = std::string(argn[x]);
			if (property == "assembly")
			{
				printf("loading assembly: %s \n", argv[x]);
				//// open our assembly
				MonoAssembly* assembly = mono_domain_assembly_open(monoDomain,
					argv[x]);
				if (assembly)
					monoImage = mono_assembly_get_image(assembly);
				else
					fprintf(stderr, "Error loading assembly: %s\n", argv[x]);
			}

			// If we have an error loading assembly then return false
			if (!monoImage)
				return false;

			if (property == "class")
			{
				printf("loading class: %s\n", argv[x]);
				split_namespace_class(argv[x], nameSpace, className);

				instanceClass = mono_class_from_name(monoImage, nameSpace.c_str(), className.c_str());
				if (!instanceClass)
					fprintf(stderr, "Error loading: namespace %s / class %s\n", nameSpace.c_str(), className.c_str());
			}

		}

		if (!instanceClass)
			return false;

		printf("Creating Instance: %s\n", className.c_str());
		pluginInstance = mono_object_new(monoDomain, instanceClass);
		if (!pluginInstance)
		{
			fprintf(stderr, "Error allocating: namespace %s / class %s\n", nameSpace.c_str(), className.c_str());
			return false;
		}

		// We will search for a constructor that takes an IntPtr as a parameter
		auto methSig = ":.ctor(intptr)";
		MonoMethodDesc* mdesc = mono_method_desc_new(methSig, true);

		auto method = mono_method_desc_search_in_class(mdesc, instanceClass);

		mono_method_desc_free(mdesc);

		if (!method)
		{
			printf("PPInstance constructor not found: `%s.%s%s`\n", nameSpace.c_str(), className.c_str(), methSig);
			return false;
		}

		printf("Constructing an instance of: %s\n", className.c_str());
		auto instance = pp_instance();

		void *args[10] = { 0 };
		args[0] = &instance;

		// finally, invoke the constructor
		MonoObject* exception = NULL;
		mono_runtime_invoke(method, pluginInstance, args, &exception);
		if (exception != nullptr)
		{
			MonoObject *other_exc = NULL;
			auto str = mono_object_to_string(exception, &other_exc);
			const char *mess = mono_string_to_utf8(str);
			printf("Exception has been thrown from constructor: `%s`", mess);
			return false;
		}

		methSig = ":Init(int,string[],string[])";
		mdesc = mono_method_desc_new(methSig, true);

		method = mono_method_desc_search_in_class(mdesc, instanceClass);

		mono_method_desc_free(mdesc);

		if (!method)
		{
			printf("PPInstance Init not found: `%s.%s%s`\n", nameSpace.c_str(), className.c_str(), methSig);
			return false;
		}

		printf("Calling Init on instance of: %s\n", className.c_str());

		args[0] = &argc;
		args[1] = parmsArgN;
		args[2] = parmsArgV;
		
		// finally, invoke the init
		
		MonoObject *result = mono_runtime_invoke(method, pluginInstance, args, &exception);
		if (exception != nullptr)
		{
			MonoObject *other_exc = NULL;
			auto str = mono_object_to_string(exception, &other_exc);
			const char *mess = mono_string_to_utf8(str);
			printf("Exception has been thrown from Init: `%s`", mess);
			return false;
		}
		else
		{
			return *(bool *)mono_object_unbox(result);
		}

		return true;
	}

private:
	MonoImage *monoImage;
	MonoClass *instanceClass;
	MonoObject *pluginInstance;
};

class PluginModule : public pp::Module {
public:
	PluginModule() : pp::Module() {}
	virtual ~PluginModule() {}

	virtual pp::Instance* CreateInstance(PP_Instance instance) {
		return new PluginInstance(instance);
	}
};



namespace pp {

	Module* CreateModule() { return new PluginModule(); }
}  // namespace pp

﻿#pragma once

#include "mono/metadata/object.h"

class FFunctionImplementation
{
public:
	static void CallReflectionFunctionImplementation(MonoObject InMonoObject, const UTF16CHAR* InFunctionName,
	                                                 MonoObject** ReturnValue, MonoObject** OutValue,
	                                                 MonoArray* InValue);
};

﻿#include "Reflection/Function/FFunctionDescriptor.h"
#include "Environment/FCSharpEnvironment.h"
#include "Macro/ClassMacro.h"
#include "Macro/FunctionMacro.h"
#include "Macro/MonoMacro.h"
#include "Macro/NamespaceMacro.h"

FFunctionDescriptor::FFunctionDescriptor(UFunction* InFunction):
	Function(InFunction),
	OriginalFunction(nullptr),
	ReturnPropertyDescriptor(nullptr)
{
	FFunctionDescriptor::Initialize();
}

FFunctionDescriptor::~FFunctionDescriptor()
{
	FFunctionDescriptor::Deinitialize();
}

void FFunctionDescriptor::Initialize()
{
	if (!Function.IsValid())
	{
		return;
	}

	PropertyDescriptors.Reserve(Function->ReturnValueOffset != MAX_uint16
		                            ? (Function->NumParms > 0
			                               ? Function->NumParms - 1
			                               : 0)
		                            : Function->NumParms);

	for (TFieldIterator<FProperty> It(Function.Get()); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		if (const auto Property = *It)
		{
			auto PropertyDescriptor = FPropertyDescriptor::Factory(Property);

			if (Property->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnPropertyDescriptor = PropertyDescriptor;

				continue;
			}

			const auto Index = PropertyDescriptors.Add(PropertyDescriptor);

			if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm) && !Property->HasAnyPropertyFlags(
				CPF_ConstParm))
			{
				OutPropertyIndexes.Add(Index);
			}
		}
	}
}

void FFunctionDescriptor::Deinitialize()
{
	for (auto& PropertyDescriptorPair : PropertyDescriptors)
	{
		delete PropertyDescriptorPair;

		PropertyDescriptorPair = nullptr;
	}

	PropertyDescriptors.Empty();

	if (ReturnPropertyDescriptor != nullptr)
	{
		delete ReturnPropertyDescriptor;

		ReturnPropertyDescriptor = nullptr;
	}
}

bool FFunctionDescriptor::CallCSharp(FFrame& Stack, void* const Z_Param__Result)
{
	const auto InParams = Stack.Locals;

	auto OutParams = Stack.OutParms;

	TArray<void*> CSharpParams;

	TArray<uint32> MallocMemoryIndexes;

	CSharpParams.Reserve(PropertyDescriptors.Num());

	for (auto Index = 0; Index < PropertyDescriptors.Num(); ++Index)
	{
		if (PropertyDescriptors[Index]->IsSharedMemory())
		{
			if (OutPropertyIndexes.Contains(Index))
			{
				OutParams = FindOutParmRec(OutParams, PropertyDescriptors[Index]->GetProperty());

				CSharpParams.Add(OutParams->PropAddr);
			}
			else
			{
				CSharpParams.Add(nullptr);

				PropertyDescriptors[Index]->Get(
					PropertyDescriptors[Index]->GetProperty()->ContainerPtrToValuePtr<void>(InParams),
					&CSharpParams[Index]);
			}
		}
		else
		{
			CSharpParams.Add(FMemory::Malloc(PropertyDescriptors[Index]->GetProperty()->GetSize()));

			PropertyDescriptors[Index]->Get(
				PropertyDescriptors[Index]->GetProperty()->ContainerPtrToValuePtr<void>(InParams),
				&CSharpParams[Index]);

			MallocMemoryIndexes.Add(Index);
		}
	}

	if (const auto FoundMonoObject = FCSharpEnvironment::GetEnvironment()->GetObject(Stack.Object))
	{
		if (const auto FoundMonoClass = FCSharpEnvironment::GetEnvironment()->GetClassDescriptor(
			Stack.Object->GetClass())->GetMonoClass())
		{
			if (const auto FoundMonoMethod = FCSharpEnvironment::GetEnvironment()->GetDomain()->
				Class_Get_Method_From_Name(FoundMonoClass, TCHAR_TO_UTF8(*Stack.Node->GetName()),
				                           PropertyDescriptors.Num()))
			{
				const auto ReturnValue = FCSharpEnvironment::GetEnvironment()->GetDomain()->Runtime_Invoke(
					FoundMonoMethod, FoundMonoObject, CSharpParams.GetData(), nullptr);

				if (ReturnValue != nullptr && ReturnPropertyDescriptor != nullptr)
				{
					if (ReturnPropertyDescriptor->IsPointerProperty())
					{
						ReturnPropertyDescriptor->Set(ReturnValue, Z_Param__Result);
					}
					else
					{
						if (const auto UnBoxResultValue = FCSharpEnvironment::GetEnvironment()->GetDomain()->
							Object_Unbox(ReturnValue))
						{
							ReturnPropertyDescriptor->Set(UnBoxResultValue, Z_Param__Result);
						}
					}
				}

				if (OutPropertyIndexes.Num() > 0)
				{
					OutParams = Stack.OutParms;

					for (const auto& Index : OutPropertyIndexes)
					{
						if (const auto OutPropertyDescriptor = PropertyDescriptors[Index])
						{
							if (!OutPropertyDescriptor->IsSharedMemory())
							{
								OutParams = FindOutParmRec(OutParams, OutPropertyDescriptor->GetProperty());

								if (OutParams != nullptr)
								{
									OutPropertyDescriptor->Set(CSharpParams[Index], OutParams->PropAddr);
								}
							}
						}
					}
				}
			}
		}
	}

	for (const auto& Index : MallocMemoryIndexes)
	{
		FMemory::Free(CSharpParams[Index]);

		CSharpParams[Index] = nullptr;
	}

	MallocMemoryIndexes.Empty();

	CSharpParams.Empty();

	return true;
}

bool FFunctionDescriptor::CallUnreal(UObject* InObject, MonoObject InMonoObject, MonoObject** ReturnValue,
                                     MonoObject** OutValue, MonoArray* InValue)
{
	auto FunctionCallspace = InObject->GetFunctionCallspace(Function.Get(), nullptr);

	const bool bIsRemote = FunctionCallspace & FunctionCallspace::Remote;

	const bool bIsLocal = FunctionCallspace & FunctionCallspace::Local;

	void* Params = Function->ParmsSize > 0 ? FMemory::Malloc(Function->ParmsSize, 16) : nullptr;

	auto ParamIndex = 0;

	for (auto Index = 0; Index < PropertyDescriptors.Num(); ++Index)
	{
		const auto& PropertyDescriptor = PropertyDescriptors[Index];

		PropertyDescriptor->GetProperty()->InitializeValue_InContainer(Params);

		if (!OutPropertyIndexes.Contains(Index))
		{
			PropertyDescriptor->Set(
				FCSharpEnvironment::GetEnvironment()->GetDomain()->
				                                      Object_Unbox(ARRAY_GET(InValue, MonoObject*, ParamIndex++)),
				PropertyDescriptor->GetProperty()->ContainerPtrToValuePtr<void>(Params));
		}
	}

	if (ReturnPropertyDescriptor != nullptr)
	{
		ReturnPropertyDescriptor->GetProperty()->InitializeValue_InContainer(Params);
	}

	if (bIsLocal)
	{
		InObject->UObject::ProcessEvent(Function.Get(), Params);

		if (ReturnPropertyDescriptor != nullptr)
		{
			ReturnPropertyDescriptor->Get(ReturnPropertyDescriptor->GetProperty()->ContainerPtrToValuePtr<void>(Params),
			                              ReturnValue);
		}

		if (OutPropertyIndexes.Num() > 0)
		{
			const auto FoundClass = FCSharpEnvironment::GetEnvironment()->GetDomain()->Class_From_Name(
				COMBINE_NAMESPACE(NAMESPACE_ROOT, NAMESPACE_FUNCTION), CLASS_INT_PTR_LIST);

			const auto FoundMethod = FCSharpEnvironment::GetEnvironment()->GetDomain()->Class_Get_Method_From_Name(
				FoundClass, FUNCTION_ADD, 1);

			const auto NewMonoObject = FCSharpEnvironment::GetEnvironment()->GetDomain()->Object_New(FoundClass);

			for (auto Index = 0; Index < OutPropertyIndexes.Num(); ++Index)
			{
				if (const auto OutPropertyDescriptor = PropertyDescriptors[OutPropertyIndexes[Index]])
				{
					auto InParams = OutPropertyDescriptor->GetProperty()->ContainerPtrToValuePtr<void>(Params);

					FCSharpEnvironment::GetEnvironment()->GetDomain()->Runtime_Invoke(
						FoundMethod, NewMonoObject, &InParams, nullptr);
				}
			}

			*OutValue = NewMonoObject;
		}
	}

	if (bIsRemote && !bIsLocal)
	{
		InObject->CallRemoteFunction(Function.Get(), Params, nullptr, nullptr);
	}

	return true;
}

FOutParmRec* FFunctionDescriptor::FindOutParmRec(FOutParmRec* OutParam, const FProperty* OutProperty)
{
	while (OutParam != nullptr)
	{
		if (OutParam->Property == OutProperty)
		{
			return OutParam;
		}

		OutParam = OutParam->NextOutParm;
	}

	return nullptr;
}

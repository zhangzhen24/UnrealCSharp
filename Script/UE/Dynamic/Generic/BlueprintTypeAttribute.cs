using System;

namespace Script.Dynamic
{
    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Enum)]
    public class BlueprintTypeAttribute : Attribute
    {
        private string Value { get; set; } = "true";
    }
}
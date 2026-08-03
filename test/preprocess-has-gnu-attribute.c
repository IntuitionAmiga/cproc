#if !__has_attribute(weak) || !__has_attribute(section) || !__has_attribute(__visibility__)
#error implemented GNU attributes must be advertised
#endif
#if !__has_attribute(aligned) || !__has_attribute(constructor) || !__has_attribute(destructor) || !__has_attribute(packed)
#error implemented GNU attributes must be advertised
#endif
#if __has_attribute(alias)
#error unsupported alias attribute must not be reported
#endif
int attributes_ok;

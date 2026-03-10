local source = debug.getinfo(1, "S").source
print("DEBUG from test_module: source =", source)
return {}

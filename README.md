Drop the folder into MyProject/Plugins, regenerate project files, build, and you are done.

---
### **Directory Layout**    


```
HL2BSPImporter/
├── HL2BSPImporter.uplugin
├── Resources/
│   └── Icon128.png
├── Config/
│   └── DefaultHL2BSPImporter.ini
├── Source/
│   └── HL2BSPImporter/
│       ├── HL2BSPImporter.Build.cs
│       ├── Public/
│       │   ├── HL2BSPImporter.h
│       │   ├── HL2BSPImporterFactory.h
│       │   ├── HL2BSPImporterSettings.h
│       │   ├── HL2BSPImporterTypes.h
│       │   ├── BspFile.h
│       │   └── HL2EntityTable.h
│       └── Private/
│           ├── HL2BSPImporter.cpp
│           ├── HL2BSPImporterFactory.cpp
│           ├── HL2BSPImporterSettings.cpp
│           ├── BspFile.cpp
│           ├── HL2EntityTable.cpp
│           └── HL2BSPImporterLog.cpp
```
---

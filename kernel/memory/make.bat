
@set PATH=D:\bin\;D:\VC\bin\amd64\;D:\COFUOS\tools\


cl /c /GS- /Oxi /Z7 /I .\include\ /I ..\util\include\ /I ..\..\util\include\ /DEBUG:FULL /D "_DEBUG" /fp:strict /TP /Wall /KERNEL /Fobin\ heap.cpp pm.cpp vm.cpp


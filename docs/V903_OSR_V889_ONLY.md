# V903: exact V889 OSR, keeping current V901 features

Owner rejected V902 occlusion quality and explicitly requested removing V891
as well. The OSR shader and CPU rules are now byte-for-byte V88986cd7261.
V890 and V891 are both absent. Pacing, framegen, RIFE, NR, UI, CPU meter/IPC90001,
projection continuity and all other V901 code remain. At the owner's additional
request the existing Off/Horizontal/Square depth dilation is available in OSR's
advanced panel and its explicit choice reaches the shared Luke depth conversion.
Off keeps the original depth; no profiles/settings are edited. No new offset, vector formula,
threshold or diagnostic capture is added. This is a local reproduction build;
character flicker with framegen is not claimed fixed. The V891-specific test is
unregistered together with the explicitly removed guard; V889 contracts remain.

CREATE TABLE Info(InfoId INTEGER PRIMARY KEY AUTOINCREMENT,InfoName TEXT NOT NULL DEFAULT '',InfoVersion TEXT NOT NULL DEFAULT '',InfoSchemaVersion INTEGER NOT NULL DEFAULT 0,InfoLastUpdate DATETIME DEFAULT CURRENT_TIMESTAMP,InfoFirstLaunch INTEGER NOT NULL DEFAULT 0,InfoTheme INTEGER NOT NULL DEFAULT 0,InfoInstallDate DATETIME DEFAULT CURRENT_TIMESTAMP,ErrorStamp DATETIME DEFAULT CURRENT_TIMESTAMP);

CREATE TABLE Errors(ErrorId INTEGER PRIMARY KEY AUTOINCREMENT,ErrorCode TEXT NOT NULL,ErrorType INTEGER NOT NULL,ErrorText TEXT NOT NULL,ErrorStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
CREATE INDEX idx_ErrorType ON Errors(ErrorType);

INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('IO_FILE_NOT_FOUND',4,'We couldn''t find the requested file. It may have been moved, renamed, or deleted.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('IO_PERMISSION_DENIED',2,'Access denied. Please check your system permissions to ensure PoseStudio can read and write to this folder.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('IO_CORRUPT_ASSET',2,'This 3D asset or project file appears to be corrupted and cannot be loaded properly.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('IO_DISK_FULL',1,'Your computer''s storage is full. Please free up some space to save your project and prevent data loss.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('GFX_HARDWARE_UNSUPPORTED',1,'Your graphics card doesn''t support the features required to render this 3D scene. Try updating your drivers or checking the hardware requirements.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('GFX_OUT_OF_VRAM',1,'The current scene is too large for your graphics card''s video memory. Try hiding complex meshes or reducing texture sizes to prevent a crash.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('GFX_SHADER_ERROR',2,'A visual rendering error occurred. Restarting the application or updating your graphics drivers might resolve this.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('GFX_CONTEXT_LOST',4,'The graphics connection was interrupted. We are attempting to recover the scene, but you may need to restart the application.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('DB_CONNECTION_FAILED',1,'PoseStudio couldn''t access its local data files. Restarting the application usually fixes this.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('DB_SCHEMA_MISMATCH',1,'Your saved data is from a different version of PoseStudio. Please update the application to ensure compatibility.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('DB_LOCKED',4,'Another program or process is currently accessing your project data. Retrying connection...');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('DB_QUERY_FAILED',2,'An unexpected error occurred while saving or retrieving your data. If this persists, please let us know.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('APP_PLUGIN_MISSING',1,'Some required application components are missing. You may need to reinstall PoseStudio to fix this.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('APP_OUT_OF_MEMORY',1,'Your computer has run out of memory. Try closing other programs to free up system resources.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('APP_THREAD_CRASH',2,'A background process encountered an unexpected error. Your current workspace should be safe, but please save your progress.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('APP_INVALID_STATE',4,'That action cannot be performed right now. Please ensure your 3D rig or scene is properly set up and try again.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('NET_OFFLINE',3,'You are currently working offline. Cloud sync and community features are temporarily unavailable.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('NET_TIMEOUT',4,'The connection to the server timed out. Please check your internet connection and try again.');
INSERT INTO Errors(ErrorCode,ErrorType,ErrorText) VALUES('NET_AUTH_FAILED',2,'Authentication failed. Please verify your login credentials to access the community hub.');

CREATE TABLE ErrorTypes(ErrorTypeId INTEGER PRIMARY KEY AUTOINCREMENT,ErrorTypeName TEXT NOT NULL,ErrorTypeStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
INSERT INTO ErrorTypes(ErrorTypeName) VALUES('Critical');
INSERT INTO ErrorTypes(ErrorTypeName) VALUES('Error');
INSERT INTO ErrorTypes(ErrorTypeName) VALUES('Info');
INSERT INTO ErrorTypes(ErrorTypeName) VALUES('Warning');

CREATE TABLE Users(UserID INTEGER PRIMARY KEY AUTOINCREMENT,UserType INTEGER NOT NULL DEFAULT 0,UserFullName TEXT NOT NULL DEFAULT '',UserCompany TEXT NOT NULL DEFAULT '',UserVendorName TEXT NOT NULL DEFAULT '',UserEmail TEXT NOT NULL DEFAULT '',UserPassword TEXT NOT NULL DEFAULT '',UserCountry INTEGER NOT NULL DEFAULT 0,UserTerms INTEGER NOT NULL DEFAULT 0,UserLic INTEGER NOT NULL DEFAULT 0,UserActive INTEGER NOT NULL DEFAULT 1,UserStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
CREATE INDEX idx_UserType ON Users(UserType);
CREATE INDEX idx_UserCountry ON Users(UserCountry);
CREATE INDEX idx_UserTerms ON Users(UserTerms);
CREATE INDEX idx_UserLic ON Users(UserLic);
CREATE INDEX idx_UserActive ON Users(UserActive);

CREATE TABLE UserTypes(UserTypeId INTEGER PRIMARY KEY AUTOINCREMENT,UserTypeName TEXT NOT NULL,UserTypeStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
INSERT INTO UserTypes(UserTypeName) VALUES('Individual');
INSERT INTO UserTypes(UserTypeName) VALUES('Business');

CREATE TABLE Preferences(PreferenceID INTEGER PRIMARY KEY AUTOINCREMENT, PreferenceName TEXT UNIQUE NOT NULL, PreferenceValue TEXT NOT NULL DEFAULT '',PreferenceStamp DATETIME DEFAULT CURRENT_TIMESTAMP);
CREATE INDEX idx_PreferenceName ON Preferences(PreferenceName);

CREATE TABLE AssetLibraries(AssetLibraryID INTEGER PRIMARY KEY AUTOINCREMENT, AssetLibraryPath TEXT NOT NULL UNIQUE, AssetLibraryEnabled INTEGER DEFAULT 1);

CREATE TABLE AssetCollections(AssetCollectionID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionName TEXT NOT NULL, AssetCollectionParentID INTEGER NOT NULL DEFAULT 0);

CREATE TABLE AssetCollectionItems(AssetCollectionItemID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionItemPath TEXT NOT NULL, AssetCollectionItemCol INTEGER NOT NULL DEFAULT 0, UNIQUE(AssetCollectionItemPath, AssetCollectionItemCol) ON CONFLICT IGNORE);
CREATE INDEX idx_AssetCollectionItemPath ON AssetCollectionItems(AssetCollectionItemPath);
CREATE INDEX idx_AssetCollectionItemCol ON AssetCollectionItems(AssetCollectionItemCol);

CREATE TABLE AssetCollectionFolders(AssetCollectionFolderID INTEGER PRIMARY KEY AUTOINCREMENT, AssetCollectionFolderPath TEXT NOT NULL, AssetCollectionFolderName TEXT NOT NULL DEFAULT '', AssetCollectionFolderCol INTEGER NOT NULL DEFAULT 0, UNIQUE(AssetCollectionFolderPath, AssetCollectionFolderCol) ON CONFLICT IGNORE);
CREATE INDEX idx_AssetCollectionFolderPath ON AssetCollectionFolders(AssetCollectionFolderPath);
CREATE INDEX idx_AssetCollectionFolderCol ON AssetCollectionFolders(AssetCollectionFolderCol);



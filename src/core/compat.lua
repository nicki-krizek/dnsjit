-- Copyright (c) 2018, OARC, Inc.
-- All rights reserved.
--
-- This file is part of dnsjit.
--
-- dnsjit is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- dnsjit is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with dnsjit.  If not, see <http://www.gnu.org/licenses/>.

-- dnsjit.core.compat
-- Cross platform compatibility support
--   require("dnsjit.core.compat_h")
--
-- This module defines various system structures so they can be exposed into
-- Lua but not really used. The size is generated at compile time to match
-- that of the structures on the platform.
-- .SS Structures
-- .TP
-- pthread_t
-- .TP
-- pthread_cond_t
-- .TP
-- pthread_mutex_t
-- .TP
-- struct sockaddr_storage
module(...,package.seeall)

require("dnsjit.core.compat_h")

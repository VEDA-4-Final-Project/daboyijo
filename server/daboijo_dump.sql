/*M!999999\- enable the sandbox mode */ 
-- MariaDB dump 10.19-11.8.6-MariaDB, for debian-linux-gnu (x86_64)
--
-- Host: localhost    Database: daboijo
-- ------------------------------------------------------
-- Server version	11.8.6-MariaDB-0+deb13u1 from Debian

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*M!100616 SET @OLD_NOTE_VERBOSITY=@@NOTE_VERBOSITY, NOTE_VERBOSITY=0 */;

--
-- Current Database: `daboijo`
--

CREATE DATABASE /*!32312 IF NOT EXISTS*/ `daboijo` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_uca1400_ai_ci */;

USE `daboijo`;

--
-- Table structure for table `care_logs`
--

DROP TABLE IF EXISTS `care_logs`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `care_logs` (
  `log_id` int(11) NOT NULL AUTO_INCREMENT,
  `camera_id` int(11) NOT NULL,
  `resident_id` int(11) DEFAULT NULL,
  `caregiver` varchar(30) DEFAULT NULL,
  `start_time` datetime NOT NULL,
  `end_time` datetime NOT NULL,
  `duration_sec` int(11) NOT NULL,
  PRIMARY KEY (`log_id`),
  KEY `resident_id` (`resident_id`),
  CONSTRAINT `care_logs_ibfk_1` FOREIGN KEY (`resident_id`) REFERENCES `residents` (`resident_id`)
) ENGINE=InnoDB AUTO_INCREMENT=8 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `care_logs`
--

SET @OLD_AUTOCOMMIT=@@AUTOCOMMIT, @@AUTOCOMMIT=0;
LOCK TABLES `care_logs` WRITE;
/*!40000 ALTER TABLE `care_logs` DISABLE KEYS */;
INSERT INTO `care_logs` VALUES
(1,3,NULL,NULL,'2026-07-10 11:39:32','2026-07-10 11:39:41',9),
(2,2,NULL,NULL,'2026-07-10 11:39:37','2026-07-10 11:39:44',7),
(3,2,NULL,NULL,'2026-07-10 14:25:08','2026-07-10 14:25:14',6),
(4,2,NULL,NULL,'2026-07-10 14:27:28','2026-07-10 14:27:33',5),
(5,2,NULL,NULL,'2026-07-16 10:56:54','2026-07-16 10:57:10',16),
(6,1,NULL,NULL,'2026-07-16 11:13:17','2026-07-16 11:13:23',6),
(7,2,NULL,NULL,'2026-07-16 11:13:35','2026-07-16 11:13:43',8);
/*!40000 ALTER TABLE `care_logs` ENABLE KEYS */;
UNLOCK TABLES;
COMMIT;
SET AUTOCOMMIT=@OLD_AUTOCOMMIT;

--
-- Table structure for table `residents`
--

DROP TABLE IF EXISTS `residents`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `residents` (
  `resident_id` int(11) NOT NULL AUTO_INCREMENT,
  `name` varchar(50) NOT NULL,
  `room` varchar(20) NOT NULL,
  `bed` varchar(20) NOT NULL,
  `camera_id` int(11) DEFAULT NULL,
  `wearable_id` varchar(32) DEFAULT NULL,
  `caregiver_id` int(11) DEFAULT NULL,
  `created_at` timestamp NULL DEFAULT current_timestamp(),
  `risk_level` enum('상','중','하') DEFAULT '중',
  `admitted_at` date DEFAULT NULL,
  `discharge_due` date DEFAULT NULL,
  `guardian_name` varchar(32) DEFAULT NULL,
  `guardian_phone` varchar(20) DEFAULT NULL,
  `guardian_relation` varchar(16) DEFAULT NULL,
  `status` varchar(20) DEFAULT '퇴원',
  `notes` text DEFAULT NULL,
  PRIMARY KEY (`resident_id`)
) ENGINE=InnoDB AUTO_INCREMENT=4 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `residents`
--

SET @OLD_AUTOCOMMIT=@@AUTOCOMMIT, @@AUTOCOMMIT=0;
LOCK TABLES `residents` WRITE;
/*!40000 ALTER TABLE `residents` DISABLE KEYS */;
INSERT INTO `residents` VALUES
(1,'전승현','301','A',1,NULL,NULL,'2026-07-10 01:48:58','중',NULL,NULL,NULL,NULL,NULL,'퇴원',NULL),
(2,'유재석','301','B',1,NULL,NULL,'2026-07-10 01:49:51','중',NULL,NULL,NULL,NULL,NULL,'퇴원',NULL);
/*!40000 ALTER TABLE `residents` ENABLE KEYS */;
UNLOCK TABLES;
COMMIT;
SET AUTOCOMMIT=@OLD_AUTOCOMMIT;

--
-- Table structure for table `roi_zones`
--

DROP TABLE IF EXISTS `roi_zones`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8mb4 */;
CREATE TABLE `roi_zones` (
  `id` int(11) NOT NULL AUTO_INCREMENT,
  `camera_id` int(11) NOT NULL,
  `roi_name` varchar(30) NOT NULL,
  `roi_points` longtext CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL CHECK (json_valid(`roi_points`)),
  `created_at` timestamp NULL DEFAULT current_timestamp(),
  PRIMARY KEY (`id`)
) ENGINE=InnoDB AUTO_INCREMENT=3 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_uca1400_ai_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Dumping data for table `roi_zones`
--

SET @OLD_AUTOCOMMIT=@@AUTOCOMMIT, @@AUTOCOMMIT=0;
LOCK TABLES `roi_zones` WRITE;
/*!40000 ALTER TABLE `roi_zones` DISABLE KEYS */;
INSERT INTO `roi_zones` VALUES
(1,1,'A','[[1,1],[1,2],[2,1],[2,2]]','2026-07-10 01:50:42'),
(2,1,'B','[[1,4],[5,2],[3,1],[4,2]]','2026-07-10 01:50:51');
/*!40000 ALTER TABLE `roi_zones` ENABLE KEYS */;
UNLOCK TABLES;
COMMIT;
SET AUTOCOMMIT=@OLD_AUTOCOMMIT;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*M!100616 SET NOTE_VERBOSITY=@OLD_NOTE_VERBOSITY */;

-- Dump completed on 2026-07-16 13:58:46

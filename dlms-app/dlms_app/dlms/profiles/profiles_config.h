/* Copyright 2023 Wirepas Ltd. All Rights Reserved.
 *
 * See file LICENSE.txt for full license details.
 *
 */

#ifndef PROFILE_CONFIG_H_
#define PROFILE_CONFIG_H_


// Daily load profile initial randomization period : 2 hours
#define DAILYLOAD_PROFILE_INITIAL_PERIOD_MS    (2 * 60 * 60 * 1000)
// Billing profile: initial randomization period and default period set as 6 hours
#define BILLING_PROFILE_INITIAL_AND_DEFAULT_PERIOD_MS   (6 * 60 * 60 * 1000)
// Event log profile: initial randomization period set as 1 hour
#define EVENT_LOG_PROFILE_INITIAL_PERIOD_MS    (60 * 60 * 1000)


// Daily load profile default period : 1 hours
#define DAILYLOAD_PROFILE_DFLT_PERIOD_MS    (60 * 60 * 1000)

// Default periods
// Blockload default period: 30 minutes
#define DEFAULT_BLOCKLOAD_PERIOD_S            (30 * 60)
// Dailyload default period: 24 hours
#define DEFAULT_DAILYLOAD_PERIOD_S        (24 * 60 * 60)

#endif /* PROFILE_CONFIG_H_ */

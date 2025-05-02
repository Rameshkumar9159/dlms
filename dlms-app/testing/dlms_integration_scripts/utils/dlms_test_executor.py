# Copyright 2024 Wirepas Ltd licensed under Apache License, Version 2.0
#
# See file LICENSE for full license details.
#
import logging
from typing import Callable, Dict, List, Union
from wirepas_dlms_tool import ErrorCodeEnum


class Result:
    def __init__(self, test_result_bool: bool, additional_text: str = None):
        """ Class to store the result of a test.

        Args:
            test_result_bool: True if the test was successful, False otherwise.
            additional_text: Additional text to explain the result of a test.
        """
        self.test_result_bool = test_result_bool
        self.additional_text = additional_text

    @classmethod
    def from_response_error_code(cls, error_code):
        if error_code == ErrorCodeEnum.RES_OK:
            return Result(True)
        if error_code == ErrorCodeEnum.RES_ERROR:
            return Result(False)
        elif error_code == ErrorCodeEnum.RES_TIMEOUT:
            return Result(False, "A timeout occured!")
        elif error_code == ErrorCodeEnum.RES_INVALID_MESSAGE:
            return Result(False, "Invalid message!")
        elif error_code == ErrorCodeEnum.RES_INVALID_KEYS:
            return Result(False, "Invalid keys!")
        elif error_code == ErrorCodeEnum.RES_MESSAGE_IS_AN_ERROR:
            return Result(False, "An error response has been received!")

        return Result(False)


class TestResult:
    def __init__(self, test_name: str, test_result_bool: bool, test_result_log: str):
        """ Class to handle test results.

        Args:
            test_name: Name of the test.
            test_result_bool: True if the test was successful, False otherwise.
            test_result_log: String to display at the end of the test and in the tests summary.
        """
        self.test_name = test_name
        self.test_result_bool = test_result_bool
        self.test_result_log = test_result_log

    @classmethod
    def from_result(cls, test_name: str, result: Result):
        """ Print whether the test passed or failed.

        Args:
            test_name: Name of the test.
            result: Result of a test execution.
        """
        if result.test_result_bool:
            test_result_log = f"{test_name}   PASSED"
        else:
            test_result_log = f"{test_name}   FAILED"

        if result.additional_text:
            test_result_log += f" ({result.additional_text})"

        return TestResult(test_name, result.test_result_bool, test_result_log)


class TestExecutor:
    """
    Class module used to execute and to print smoothly the test results of each of the meters.
    All test results for the meter that have been tested can be displayed.
    """
    results: Dict[int, List[TestResult]] = {}  # Test results of each node.

    def execute_test(self, meter, test_function: Callable, test_name: str = None,
                     test_conditions: Union[bool, List[Callable]] = None, **kwargs) -> TestResult:
        """ Execute a test for the meter.
        Then store the test result of the meter then return it.

        Args:
            meter: Meter to be tested.
            test_function: Test function to execute that returns either the test result
                or a tuple containing the test result and a additional message
                to be printed in the tests summary.
            test_name: Name of the test to be executed.
            test_conditions: Either a boolean asserting that the test conditions passed or
                a list of functions to be executed that need to return True
                before the execution of a test if it is provided.
                Default to None if the test do not require conditions.
        """
        if not test_name:
            test_name = test_function.__name__

        fail_result = Result(test_result_bool=False, additional_text="Conditions for the test are not met!")
        failed_test_result = TestResult.from_result(test_name, fail_result)

        # Verify that the test conditions are met
        if isinstance(test_conditions, bool):
            if not test_conditions:
                return self.add_test_result(meter, failed_test_result)
            logging.info("===> Test conditions for %s are successful !", test_name)
        else:
            try:
                if test_conditions:
                    for test_condition in test_conditions:
                        logging.info("Executing test conditions for %s!", test_name)
                        condition_result = test_condition(meter)
                        if not condition_result.test_result_bool:
                            if condition_result.additional_text:
                                return self.add_test_result(meter, TestResult.from_result(test_name, condition_result))
                            return self.add_test_result(meter, failed_test_result)

                    logging.info("===> Test conditions for %s are successful !", test_name)
            except Exception as e:
                logging.exception(e)
                return failed_test_result

        # Execute the test.
        logging.info("===> %s is being tested !", test_name)
        result = Result(False)
        try:
            result = test_function(meter=meter, **kwargs)
        except Exception as e:  # catch exception, the test failed.
            logging.exception(f"The test {test_name} failed => {e}")

        test_result = TestResult.from_result(test_name, result)
        return self.add_test_result(meter, test_result)

    def add_test_result(self, meter, test_result: TestResult) -> TestResult:
        """ Store a test result by node id. """
        logging.info(test_result.test_result_log)

        # Store the test result.
        if meter.node_id not in self.results:
            self.results[meter.node_id] = []
        self.results[meter.node_id].append(test_result)

        logging.info("-" * 50)
        return test_result

    def print_all_test_results(self):
        """ Do a summary of the tests results, meter by meter. """
        nb_all_passed_tests = 0

        if not self.results:
            logging.warning("No tests were executed!")
            return

        logging.info("Tag of the test and their test result (with additional informations on failure).")
        for node_id, node_result in self.results.items():
            passed_tests = 0
            logging.info(f"Node {node_id} tests result:")
            for test_result in node_result:
                if test_result.test_result_bool:
                    passed_tests += 1
                logging.info(test_result.test_result_log)

            nb_all_passed_tests += passed_tests
            logging.info("===> %d/%d tests passed successfully ! <===\n",
                         passed_tests, len(node_result))

        # Do a global summary of the nodes tests if there are multiple nodes
        if len(self.results) > 1:
            logging.info("===> In total %d/%d tests passed successfully ! <===",
                         nb_all_passed_tests, len(self.results) * len(node_result))
